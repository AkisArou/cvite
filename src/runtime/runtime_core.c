#include "runtime_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

void cvite_internal_lock(cvite_runtime *runtime)
{
    while (atomic_flag_test_and_set_explicit(
        &runtime->writer_lock, memory_order_acquire)) {
    }
}

void cvite_internal_unlock(cvite_runtime *runtime)
{
    atomic_flag_clear_explicit(&runtime->writer_lock, memory_order_release);
}

void cvite_internal_snapshot_reader_enter(cvite_runtime *runtime)
{
    for (;;) {
        while (atomic_load_explicit(
            &runtime->snapshot_collection_gate, memory_order_seq_cst)) {
            atomic_signal_fence(memory_order_seq_cst);
        }

        (void)atomic_fetch_add_explicit(
            &runtime->active_snapshot_readers,
            UINT64_C(1),
            memory_order_seq_cst);
        if (!atomic_load_explicit(
                &runtime->snapshot_collection_gate, memory_order_seq_cst)) {
            return;
        }

        (void)atomic_fetch_sub_explicit(
            &runtime->active_snapshot_readers,
            UINT64_C(1),
            memory_order_seq_cst);
    }
}

void cvite_internal_snapshot_reader_leave(cvite_runtime *runtime)
{
    const uint_fast64_t previous = atomic_fetch_sub_explicit(
        &runtime->active_snapshot_readers,
        UINT64_C(1),
        memory_order_seq_cst);
    if (previous == 0U) {
        abort();
    }
}

void cvite_error_clear(cvite_error *error)
{
    if (error == NULL) {
        return;
    }

    memset(error, 0, sizeof(*error));
    error->status = CVITE_STATUS_OK;
}

cvite_status cvite_internal_fail(
    cvite_error *error,
    cvite_status status,
    cvite_id symbol,
    cvite_id expected,
    cvite_id actual,
    const char *format,
    ...)
{
    if (error != NULL) {
        va_list arguments;

        cvite_error_clear(error);
        error->status = status;
        error->symbol = symbol;
        error->expected_fingerprint = expected;
        error->actual_fingerprint = actual;

        va_start(arguments, format);
        (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
        va_end(arguments);
    }

    return status;
}

char *cvite_internal_duplicate_string(const char *source)
{
    size_t length = 0U;
    char *copy = NULL;

    if (source == NULL) {
        source = "<unnamed>";
    }

    length = strlen(source);
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, source, length + 1U);
    }
    return copy;
}

bool cvite_internal_is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

void *cvite_internal_allocate_aligned(size_t alignment, size_t size)
{
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void *memory = NULL;
    size_t effective_alignment = alignment;

    if (effective_alignment < sizeof(void *)) {
        effective_alignment = sizeof(void *);
    }
    if (posix_memalign(&memory, effective_alignment, size) != 0) {
        return NULL;
    }
    return memory;
#endif
}

void cvite_internal_free_aligned(void *memory)
{
#if defined(_WIN32)
    _aligned_free(memory);
#else
    free(memory);
#endif
}

size_t cvite_internal_find_function_index(
    const cvite_runtime *runtime,
    cvite_id id)
{
    size_t index = 0U;

    for (index = 0U; index < runtime->function_count; ++index) {
        if (cvite_id_equal(runtime->functions[index].id, id)) {
            return index;
        }
    }
    return SIZE_MAX;
}

size_t cvite_internal_find_storage_index(
    const cvite_runtime *runtime,
    cvite_id id)
{
    size_t index = 0U;

    for (index = 0U; index < runtime->storage_count; ++index) {
        if (cvite_id_equal(runtime->storage[index].id, id)) {
            return index;
        }
    }
    return SIZE_MAX;
}

cvite_dispatch_snapshot *cvite_internal_allocate_snapshot(size_t slot_count)
{
    cvite_dispatch_snapshot *snapshot = NULL;
    size_t total_size = 0U;

    if (slot_count >
        (SIZE_MAX - sizeof(*snapshot)) / sizeof(snapshot->targets[0])) {
        return NULL;
    }

    total_size = sizeof(*snapshot) +
        slot_count * sizeof(snapshot->targets[0]);
    snapshot = (cvite_dispatch_snapshot *)calloc(1U, total_size);
    if (snapshot != NULL) {
        snapshot->slot_count = slot_count;
    }
    return snapshot;
}

static cvite_status cvite_grow_records(
    void **records,
    size_t *capacity,
    size_t record_size,
    const char *description,
    cvite_error *error)
{
    size_t new_capacity = *capacity == 0U ? 8U : *capacity * 2U;
    void *grown = NULL;

    if (new_capacity < *capacity || new_capacity > SIZE_MAX / record_size) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "%s registry capacity overflow",
            description);
    }

    grown = realloc(*records, new_capacity * record_size);
    if (grown == NULL) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "could not grow %s registry",
            description);
    }

    *records = grown;
    *capacity = new_capacity;
    return CVITE_STATUS_OK;
}

cvite_status cvite_internal_grow_functions(
    cvite_runtime *runtime,
    cvite_error *error)
{
    return cvite_grow_records(
        (void **)&runtime->functions,
        &runtime->function_capacity,
        sizeof(runtime->functions[0]),
        "function",
        error);
}

cvite_status cvite_internal_grow_storage(
    cvite_runtime *runtime,
    cvite_error *error)
{
    return cvite_grow_records(
        (void **)&runtime->storage,
        &runtime->storage_capacity,
        sizeof(runtime->storage[0]),
        "storage",
        error);
}

cvite_status cvite_runtime_create(cvite_runtime **runtime, cvite_error *error)
{
    cvite_runtime *created = NULL;
    cvite_dispatch_snapshot *initial = NULL;

    cvite_error_clear(error);
    if (runtime == NULL) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "runtime output pointer is null");
    }

    *runtime = NULL;
    created = (cvite_runtime *)calloc(1U, sizeof(*created));
    initial = cvite_internal_allocate_snapshot(0U);
    if (created == NULL || initial == NULL) {
        free(created);
        free(initial);
        return cvite_internal_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "could not allocate runtime");
    }

    created->writer_lock = (atomic_flag)ATOMIC_FLAG_INIT;
    atomic_init(&created->snapshot_collection_gate, false);
    atomic_init(&created->active_snapshot_readers, UINT64_C(0));
    atomic_init(&created->active_snapshot, initial);
    *runtime = created;
    return CVITE_STATUS_OK;
}

void cvite_runtime_destroy(cvite_runtime *runtime)
{
    cvite_dispatch_snapshot *snapshot = NULL;
    size_t index = 0U;

    if (runtime == NULL) {
        return;
    }

    free(atomic_load_explicit(
        &runtime->active_snapshot, memory_order_relaxed));
    snapshot = runtime->retired_snapshots;
    while (snapshot != NULL) {
        cvite_dispatch_snapshot *next = snapshot->retired_next;
        free(snapshot);
        snapshot = next;
    }

    for (index = 0U; index < runtime->function_count; ++index) {
        free(runtime->functions[index].debug_name);
    }
    for (index = 0U; index < runtime->storage_count; ++index) {
        free(runtime->storage[index].debug_name);
        cvite_internal_free_aligned(runtime->storage[index].address);
    }

    free(runtime->functions);
    free(runtime->storage);
    free(runtime);
}

cvite_status cvite_runtime_seal(cvite_runtime *runtime, cvite_error *error)
{
    cvite_error_clear(error);
    if (runtime == NULL) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "runtime is null");
    }

    cvite_internal_lock(runtime);
    runtime->sealed = true;
    cvite_internal_unlock(runtime);
    return CVITE_STATUS_OK;
}

cvite_status cvite_runtime_collect_retired(
    cvite_runtime *runtime,
    size_t *reclaimed_count,
    cvite_error *error)
{
    cvite_dispatch_snapshot *snapshot = NULL;
    size_t reclaimed = 0U;
    bool expected = false;

    cvite_error_clear(error);
    if (runtime == NULL || reclaimed_count == NULL) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid retired-snapshot collection request");
    }

    *reclaimed_count = 0U;
    cvite_internal_lock(runtime);
    if (!atomic_compare_exchange_strong_explicit(
            &runtime->snapshot_collection_gate,
            &expected,
            true,
            memory_order_seq_cst,
            memory_order_seq_cst)) {
        cvite_internal_unlock(runtime);
        return CVITE_STATUS_OK;
    }

    if (atomic_load_explicit(
            &runtime->active_snapshot_readers,
            memory_order_seq_cst) != UINT64_C(0)) {
        atomic_store_explicit(
            &runtime->snapshot_collection_gate,
            false,
            memory_order_seq_cst);
        cvite_internal_unlock(runtime);
        return CVITE_STATUS_OK;
    }

    snapshot = runtime->retired_snapshots;
    runtime->retired_snapshots = NULL;
    while (snapshot != NULL) {
        cvite_dispatch_snapshot *next = snapshot->retired_next;
        free(snapshot);
        snapshot = next;
        reclaimed += 1U;
    }

    atomic_store_explicit(
        &runtime->snapshot_collection_gate,
        false,
        memory_order_seq_cst);
    cvite_internal_unlock(runtime);
    *reclaimed_count = reclaimed;
    return CVITE_STATUS_OK;
}

uint64_t cvite_runtime_generation(const cvite_runtime *runtime)
{
    cvite_runtime *mutable_runtime = (cvite_runtime *)runtime;
    cvite_dispatch_snapshot *snapshot = NULL;
    uint64_t generation = 0U;

    if (runtime == NULL) {
        return 0U;
    }

    cvite_internal_snapshot_reader_enter(mutable_runtime);
    snapshot = atomic_load_explicit(
        &runtime->active_snapshot, memory_order_acquire);
    generation = snapshot->generation;
    cvite_internal_snapshot_reader_leave(mutable_runtime);
    return generation;
}

size_t cvite_runtime_function_count(const cvite_runtime *runtime)
{
    return runtime != NULL ? runtime->function_count : 0U;
}
