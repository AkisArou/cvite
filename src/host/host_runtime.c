#include "cvite/host.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CVITE_HOST_INVALID_SLOT UINT64_MAX

typedef struct cvite_host_function_record {
    cvite_host_function function;
} cvite_host_function_record;

typedef struct cvite_host_storage_record {
    cvite_host_storage storage;
} cvite_host_storage_record;

static atomic_flag cvite_host_lock_flag = ATOMIC_FLAG_INIT;
static _Atomic(cvite_runtime *) cvite_host_active_runtime = NULL;
static atomic_bool cvite_host_is_sealed = false;
static atomic_bool cvite_host_quiescence_gate = false;
static atomic_uint_fast64_t cvite_host_active_calls = 0U;
static cvite_host_function_record *cvite_host_function_records = NULL;
static size_t cvite_host_function_record_count = 0U;
static size_t cvite_host_function_record_capacity = 0U;
static cvite_host_storage_record *cvite_host_storage_records = NULL;
static size_t cvite_host_storage_record_count = 0U;
static size_t cvite_host_storage_record_capacity = 0U;

static void cvite_host_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
        &cvite_host_lock_flag, memory_order_acquire)) {
    }
}

static void cvite_host_unlock(void)
{
    atomic_flag_clear_explicit(&cvite_host_lock_flag, memory_order_release);
}

static cvite_status cvite_host_fail(
    cvite_error *error,
    cvite_status status,
    const char *message)
{
    cvite_error_clear(error);
    if (error != NULL) {
        error->status = status;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return status;
}

static cvite_status cvite_host_storage_layout_mismatch(
    cvite_error *error,
    const cvite_host_storage *existing,
    const cvite_storage_definition *definition)
{
    cvite_error_clear(error);
    if (error != NULL) {
        error->status = CVITE_STATUS_LAYOUT_MISMATCH;
        error->symbol = definition->id;
        error->expected_fingerprint = existing->layout_fingerprint;
        error->actual_fingerprint = definition->layout_fingerprint;
        (void)snprintf(
            error->message,
            sizeof(error->message),
            "storage '%s' changed layout",
            existing->debug_name);
    }
    return CVITE_STATUS_LAYOUT_MISMATCH;
}

static void cvite_host_fatal(const char *operation, const cvite_error *error)
{
    const char *message = "unknown CVite host error";
    if (error != NULL && error->message[0] != '\0') {
        message = error->message;
    }

    (void)fprintf(stderr, "cvite host: %s failed: %s\n", operation, message);
    abort();
}

static bool cvite_host_is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static cvite_runtime *cvite_host_ensure_runtime_locked(cvite_error *error)
{
    cvite_runtime *runtime = atomic_load_explicit(
        &cvite_host_active_runtime, memory_order_acquire);
    if (runtime != NULL) {
        return runtime;
    }

    if (cvite_runtime_create(&runtime, error) != CVITE_STATUS_OK) {
        return NULL;
    }

    atomic_store_explicit(
        &cvite_host_active_runtime, runtime, memory_order_release);
    return runtime;
}

static cvite_status cvite_host_grow_registry_locked(
    void **records,
    size_t *capacity,
    size_t record_size,
    const char *description,
    cvite_error *error)
{
    size_t new_capacity = *capacity == 0U ? 16U : *capacity * 2U;
    void *grown = NULL;

    if (new_capacity < *capacity || new_capacity > SIZE_MAX / record_size) {
        return cvite_host_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            "host registry capacity overflow");
    }

    grown = realloc(*records, new_capacity * record_size);
    if (grown == NULL) {
        char message[128];
        (void)snprintf(
            message,
            sizeof(message),
            "could not grow the host %s registry",
            description);
        return cvite_host_fail(error, CVITE_STATUS_OUT_OF_MEMORY, message);
    }

    *records = grown;
    *capacity = new_capacity;
    return CVITE_STATUS_OK;
}

static cvite_status cvite_host_seal(cvite_error *error)
{
    cvite_status status = CVITE_STATUS_OK;
    cvite_runtime *runtime = NULL;

    if (atomic_load_explicit(&cvite_host_is_sealed, memory_order_acquire)) {
        return CVITE_STATUS_OK;
    }

    cvite_host_lock();
    if (!atomic_load_explicit(&cvite_host_is_sealed, memory_order_relaxed)) {
        runtime = cvite_host_ensure_runtime_locked(error);
        if (runtime == NULL) {
            status = error != NULL ? error->status : CVITE_STATUS_OUT_OF_MEMORY;
        } else {
            status = cvite_runtime_seal(runtime, error);
            if (status == CVITE_STATUS_OK) {
                atomic_store_explicit(
                    &cvite_host_is_sealed, true, memory_order_release);
            }
        }
    }
    cvite_host_unlock();
    return status;
}

static size_t cvite_host_find_storage_locked(cvite_id id)
{
    size_t index = 0U;

    for (index = 0U; index < cvite_host_storage_record_count; ++index) {
        if (cvite_id_equal(cvite_host_storage_records[index].storage.id, id)) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool cvite_host_storage_layout_matches(
    const cvite_host_storage *storage,
    const cvite_storage_definition *definition)
{
    return cvite_id_equal(
               storage->layout_fingerprint,
               definition->layout_fingerprint) &&
        storage->size == definition->size &&
        storage->alignment == definition->alignment;
}

cvite_status cvite_host_register_function(
    const cvite_function_definition *definition,
    cvite_host_function *function,
    cvite_error *error)
{
    cvite_runtime *runtime = NULL;
    cvite_status status = CVITE_STATUS_OK;
    size_t slot = SIZE_MAX;

    cvite_error_clear(error);
    if (definition == NULL || function == NULL ||
        cvite_id_is_zero(definition->id) ||
        cvite_id_is_zero(definition->abi_fingerprint) ||
        definition->initial_target == NULL ||
        definition->debug_name == NULL ||
        definition->debug_name[0] == '\0') {
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid host function definition");
    }

    cvite_host_lock();
    if (atomic_load_explicit(&cvite_host_is_sealed, memory_order_relaxed)) {
        cvite_host_unlock();
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "a module tried to register after the host was sealed");
    }

    runtime = cvite_host_ensure_runtime_locked(error);
    if (runtime == NULL) {
        cvite_host_unlock();
        return error != NULL ? error->status : CVITE_STATUS_OUT_OF_MEMORY;
    }

    if (cvite_host_function_record_count ==
        cvite_host_function_record_capacity) {
        status = cvite_host_grow_registry_locked(
            (void **)&cvite_host_function_records,
            &cvite_host_function_record_capacity,
            sizeof(cvite_host_function_records[0]),
            "function",
            error);
        if (status != CVITE_STATUS_OK) {
            cvite_host_unlock();
            return status;
        }
    }

    status = cvite_runtime_register_function(
        runtime, definition, &slot, error);
    if (status != CVITE_STATUS_OK) {
        cvite_host_unlock();
        return status;
    }

    *function = (cvite_host_function){
        definition->id,
        definition->abi_fingerprint,
        slot,
        definition->debug_name,
    };
    cvite_host_function_records[cvite_host_function_record_count].function =
        *function;
    cvite_host_function_record_count += 1U;
    cvite_host_unlock();
    return CVITE_STATUS_OK;
}

cvite_status cvite_host_register_storage(
    const cvite_storage_definition *definition,
    void *address,
    cvite_host_storage *storage,
    cvite_error *error)
{
    cvite_status status = CVITE_STATUS_OK;
    size_t index = SIZE_MAX;

    cvite_error_clear(error);
    if (definition == NULL || storage == NULL || address == NULL ||
        cvite_id_is_zero(definition->id) ||
        cvite_id_is_zero(definition->layout_fingerprint) ||
        definition->size == 0U ||
        !cvite_host_is_power_of_two(definition->alignment) ||
        ((uintptr_t)address & (uintptr_t)(definition->alignment - 1U)) != 0U ||
        definition->debug_name == NULL ||
        definition->debug_name[0] == '\0') {
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid host storage definition");
    }

    cvite_host_lock();
    if (atomic_load_explicit(&cvite_host_is_sealed, memory_order_relaxed)) {
        cvite_host_unlock();
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "a storage definition tried to register after the host was sealed");
    }

    index = cvite_host_find_storage_locked(definition->id);
    if (index != SIZE_MAX) {
        const cvite_host_storage *existing =
            &cvite_host_storage_records[index].storage;
        if (!cvite_host_storage_layout_matches(existing, definition)) {
            cvite_host_unlock();
            return cvite_host_storage_layout_mismatch(
                error, existing, definition);
        }
        if (existing->address != address) {
            cvite_host_unlock();
            return cvite_host_fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "one storage ID was bound to two native addresses");
        }
        *storage = *existing;
        cvite_host_unlock();
        return CVITE_STATUS_OK;
    }

    if (cvite_host_storage_record_count == cvite_host_storage_record_capacity) {
        status = cvite_host_grow_registry_locked(
            (void **)&cvite_host_storage_records,
            &cvite_host_storage_record_capacity,
            sizeof(cvite_host_storage_records[0]),
            "storage",
            error);
        if (status != CVITE_STATUS_OK) {
            cvite_host_unlock();
            return status;
        }
    }

    *storage = (cvite_host_storage){
        definition->id,
        definition->layout_fingerprint,
        address,
        definition->size,
        definition->alignment,
        definition->debug_name,
    };
    cvite_host_storage_records[cvite_host_storage_record_count].storage =
        *storage;
    cvite_host_storage_record_count += 1U;
    cvite_host_unlock();
    return CVITE_STATUS_OK;
}

cvite_status cvite_host_require_storage(
    const cvite_storage_definition *definition,
    cvite_host_storage *storage,
    cvite_error *error)
{
    cvite_status status = CVITE_STATUS_OK;
    size_t index = SIZE_MAX;

    cvite_error_clear(error);
    if (definition == NULL || storage == NULL ||
        cvite_id_is_zero(definition->id) ||
        cvite_id_is_zero(definition->layout_fingerprint) ||
        definition->size == 0U ||
        !cvite_host_is_power_of_two(definition->alignment)) {
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid host storage requirement");
    }

    status = cvite_host_seal(error);
    if (status != CVITE_STATUS_OK) {
        return status;
    }

    cvite_host_lock();
    index = cvite_host_find_storage_locked(definition->id);
    if (index == SIZE_MAX) {
        cvite_host_unlock();
        return cvite_host_fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            "candidate references unknown persistent storage");
    }

    *storage = cvite_host_storage_records[index].storage;
    if (!cvite_host_storage_layout_matches(storage, definition)) {
        cvite_host_unlock();
        return cvite_host_storage_layout_mismatch(error, storage, definition);
    }
    cvite_host_unlock();
    return CVITE_STATUS_OK;
}

uint64_t __cvite_host_register_function(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t abi_high,
    uint64_t abi_low,
    cvite_function_pointer initial_target,
    const char *debug_name)
{
    const cvite_function_definition definition = {
        CVITE_ID(id_high, id_low),
        CVITE_ID(abi_high, abi_low),
        initial_target,
        debug_name,
    };
    cvite_host_function function;
    cvite_error error;

    if (cvite_host_register_function(&definition, &function, &error) !=
        CVITE_STATUS_OK) {
        cvite_host_fatal("function registration", &error);
    }
    if (function.slot > (size_t)UINT64_MAX) {
        cvite_host_fail(
            &error,
            CVITE_STATUS_INVALID_STATE,
            "function slot does not fit the compiler ABI");
        cvite_host_fatal("function registration", &error);
    }
    return (uint64_t)function.slot;
}

void *__cvite_host_register_storage(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t layout_high,
    uint64_t layout_low,
    uint64_t encoded_size,
    uint64_t encoded_alignment,
    void *address,
    const char *debug_name)
{
    cvite_storage_definition definition;
    cvite_host_storage storage;
    cvite_error error;

    if (encoded_size > (uint64_t)SIZE_MAX ||
        encoded_alignment > (uint64_t)SIZE_MAX) {
        cvite_host_fail(
            &error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "storage size or alignment does not fit the host ABI");
        cvite_host_fatal("storage registration", &error);
    }

    definition = (cvite_storage_definition){
        CVITE_ID(id_high, id_low),
        CVITE_ID(layout_high, layout_low),
        (size_t)encoded_size,
        (size_t)encoded_alignment,
        NULL,
        debug_name,
    };
    if (cvite_host_register_storage(
            &definition, address, &storage, &error) != CVITE_STATUS_OK) {
        cvite_host_fatal("storage registration", &error);
    }
    return storage.address;
}

void __cvite_host_call_enter(void)
{
    for (;;) {
        while (atomic_load_explicit(
            &cvite_host_quiescence_gate, memory_order_acquire)) {
        }

        const uint_fast64_t previous = atomic_fetch_add_explicit(
            &cvite_host_active_calls, 1U, memory_order_acq_rel);
        if (previous == UINT_FAST64_MAX) {
            (void)atomic_fetch_sub_explicit(
                &cvite_host_active_calls, 1U, memory_order_acq_rel);
            cvite_host_fatal("call-scope entry", NULL);
        }

        if (!atomic_load_explicit(
                &cvite_host_quiescence_gate, memory_order_acquire)) {
            return;
        }

        (void)atomic_fetch_sub_explicit(
            &cvite_host_active_calls, 1U, memory_order_acq_rel);
    }
}

void __cvite_host_call_leave(void)
{
    const uint_fast64_t previous = atomic_fetch_sub_explicit(
        &cvite_host_active_calls, 1U, memory_order_acq_rel);
    if (previous == 0U) {
        (void)atomic_fetch_add_explicit(
            &cvite_host_active_calls, 1U, memory_order_relaxed);
        cvite_host_fatal("call-scope exit", NULL);
    }
}

bool cvite_host_try_begin_quiescence(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &cvite_host_quiescence_gate,
            &expected,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return false;
    }

    if (atomic_load_explicit(
            &cvite_host_active_calls, memory_order_acquire) != 0U) {
        atomic_store_explicit(
            &cvite_host_quiescence_gate, false, memory_order_release);
        return false;
    }
    return true;
}

void cvite_host_end_quiescence(void)
{
    atomic_store_explicit(
        &cvite_host_quiescence_gate, false, memory_order_release);
}

uint64_t cvite_host_active_call_count(void)
{
    return (uint64_t)atomic_load_explicit(
        &cvite_host_active_calls, memory_order_acquire);
}

cvite_function_pointer __cvite_host_target_at(uint64_t encoded_slot)
{
    cvite_runtime *runtime = NULL;
    cvite_function_pointer target = NULL;
    cvite_error error;

    if (encoded_slot == CVITE_HOST_INVALID_SLOT ||
        encoded_slot > (uint64_t)SIZE_MAX) {
        cvite_host_fail(
            &error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "compiler-generated wrapper contains an invalid function slot");
        cvite_host_fatal("function dispatch", &error);
    }

    if (cvite_host_seal(&error) != CVITE_STATUS_OK) {
        cvite_host_fatal("runtime sealing", &error);
    }

    runtime = atomic_load_explicit(
        &cvite_host_active_runtime, memory_order_acquire);
    target = cvite_runtime_target_at(runtime, (size_t)encoded_slot);
    if (target == NULL) {
        cvite_host_fail(
            &error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            "compiler-generated wrapper references an unknown function slot");
        cvite_host_fatal("function dispatch", &error);
    }
    return target;
}

cvite_function_pointer __cvite_host_target_for(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t abi_high,
    uint64_t abi_low)
{
    const cvite_id id = CVITE_ID(id_high, id_low);
    const cvite_id abi = CVITE_ID(abi_high, abi_low);
    cvite_host_function function;
    cvite_error error;
    size_t index = 0U;
    size_t match = SIZE_MAX;
    size_t match_count = 0U;

    if (cvite_host_seal(&error) != CVITE_STATUS_OK) {
        cvite_host_fatal("runtime sealing", &error);
    }

    cvite_host_lock();
    for (index = 0U; index < cvite_host_function_record_count; ++index) {
        if (cvite_id_equal(
                cvite_host_function_records[index].function.id,
                id)) {
            match = index;
            match_count += 1U;
        }
    }

    if (match_count == 0U) {
        cvite_host_unlock();
        cvite_host_fail(
            &error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            "candidate references an unknown stable function ID");
        cvite_host_fatal("candidate dispatch", &error);
    }
    if (match_count > 1U) {
        cvite_host_unlock();
        cvite_host_fail(
            &error,
            CVITE_STATUS_DUPLICATE_SYMBOL,
            "candidate function ID is ambiguous");
        cvite_host_fatal("candidate dispatch", &error);
    }

    function = cvite_host_function_records[match].function;
    cvite_host_unlock();
    if (!cvite_id_equal(function.abi_fingerprint, abi)) {
        cvite_host_fail(
            &error,
            CVITE_STATUS_ABI_MISMATCH,
            "candidate call crosses an incompatible function ABI");
        cvite_host_fatal("candidate dispatch", &error);
    }

    return __cvite_host_target_at((uint64_t)function.slot);
}

void *__cvite_host_storage_for(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t layout_high,
    uint64_t layout_low,
    uint64_t encoded_size,
    uint64_t encoded_alignment,
    const char *debug_name)
{
    cvite_storage_definition definition;
    cvite_host_storage storage;
    cvite_error error;

    if (encoded_size > (uint64_t)SIZE_MAX ||
        encoded_alignment > (uint64_t)SIZE_MAX) {
        cvite_host_fail(
            &error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "storage requirement does not fit the host ABI");
        cvite_host_fatal("candidate storage resolution", &error);
    }

    definition = (cvite_storage_definition){
        CVITE_ID(id_high, id_low),
        CVITE_ID(layout_high, layout_low),
        (size_t)encoded_size,
        (size_t)encoded_alignment,
        NULL,
        debug_name,
    };
    if (cvite_host_require_storage(&definition, &storage, &error) !=
        CVITE_STATUS_OK) {
        cvite_host_fatal("candidate storage resolution", &error);
    }
    return storage.address;
}

cvite_status cvite_host_find_function(
    const char *debug_name,
    cvite_host_function *function,
    cvite_error *error)
{
    size_t index = 0U;
    size_t match = SIZE_MAX;
    size_t match_count = 0U;

    cvite_error_clear(error);
    if (debug_name == NULL || debug_name[0] == '\0' || function == NULL) {
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid host function lookup");
    }

    cvite_host_lock();
    for (index = 0U; index < cvite_host_function_record_count; ++index) {
        if (strcmp(
                cvite_host_function_records[index].function.debug_name,
                debug_name) == 0) {
            match = index;
            match_count += 1U;
        }
    }

    if (match_count == 0U) {
        cvite_host_unlock();
        return cvite_host_fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            "host function name is not registered");
    }
    if (match_count > 1U) {
        cvite_host_unlock();
        return cvite_host_fail(
            error,
            CVITE_STATUS_DUPLICATE_SYMBOL,
            "host function name is ambiguous; use its stable ID");
    }

    *function = cvite_host_function_records[match].function;
    cvite_host_unlock();
    return CVITE_STATUS_OK;
}

size_t cvite_host_storage_count(void)
{
    size_t count = 0U;

    cvite_host_lock();
    count = cvite_host_storage_record_count;
    cvite_host_unlock();
    return count;
}

cvite_status cvite_host_storage_at(
    size_t index,
    cvite_host_storage *storage,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (storage == NULL) {
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "host storage output is null");
    }

    cvite_host_lock();
    if (index >= cvite_host_storage_record_count) {
        cvite_host_unlock();
        return cvite_host_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "host storage index is out of range");
    }
    *storage = cvite_host_storage_records[index].storage;
    cvite_host_unlock();
    return CVITE_STATUS_OK;
}

cvite_status cvite_host_apply_patch(
    const cvite_patch *patch,
    cvite_error *error)
{
    cvite_runtime *runtime = NULL;
    cvite_status status = cvite_host_seal(error);
    if (status != CVITE_STATUS_OK) {
        return status;
    }

    runtime = atomic_load_explicit(
        &cvite_host_active_runtime, memory_order_acquire);
    return cvite_runtime_apply_patch(runtime, patch, error);
}

uint64_t cvite_host_generation(void)
{
    cvite_runtime *runtime = atomic_load_explicit(
        &cvite_host_active_runtime, memory_order_acquire);
    return runtime != NULL ? cvite_runtime_generation(runtime) : 0U;
}
