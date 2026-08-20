#include "cvite/host.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CVITE_HOST_INVALID_SLOT UINT64_MAX

typedef struct cvite_host_record {
    cvite_host_function function;
} cvite_host_record;

static atomic_flag cvite_host_lock_flag = ATOMIC_FLAG_INIT;
static _Atomic(cvite_runtime *) cvite_host_active_runtime = NULL;
static atomic_bool cvite_host_is_sealed = false;
static cvite_host_record *cvite_host_records = NULL;
static size_t cvite_host_record_count = 0U;
static size_t cvite_host_record_capacity = 0U;

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

static void cvite_host_fatal(const char *operation, const cvite_error *error)
{
    const char *message = "unknown CVite host error";
    if (error != NULL && error->message[0] != '\0') {
        message = error->message;
    }

    (void)fprintf(stderr, "cvite host: %s failed: %s\n", operation, message);
    abort();
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

static cvite_status cvite_host_grow_records_locked(cvite_error *error)
{
    size_t new_capacity = cvite_host_record_capacity == 0U
        ? 16U
        : cvite_host_record_capacity * 2U;
    cvite_host_record *grown = NULL;

    if (new_capacity < cvite_host_record_capacity ||
        new_capacity > SIZE_MAX / sizeof(*grown)) {
        return cvite_host_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            "function registry capacity overflow");
    }

    grown = (cvite_host_record *)realloc(
        cvite_host_records, new_capacity * sizeof(*grown));
    if (grown == NULL) {
        return cvite_host_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            "could not grow the host function registry");
    }

    cvite_host_records = grown;
    cvite_host_record_capacity = new_capacity;
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

    if (cvite_host_record_count == cvite_host_record_capacity) {
        status = cvite_host_grow_records_locked(error);
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
    cvite_host_records[cvite_host_record_count].function = *function;
    cvite_host_record_count += 1U;
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
    for (index = 0U; index < cvite_host_record_count; ++index) {
        if (cvite_id_equal(cvite_host_records[index].function.id, id)) {
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

    function = cvite_host_records[match].function;
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
    for (index = 0U; index < cvite_host_record_count; ++index) {
        if (strcmp(cvite_host_records[index].function.debug_name, debug_name) == 0) {
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

    *function = cvite_host_records[match].function;
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
