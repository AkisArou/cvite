#ifndef CVITE_RUNTIME_INTERNAL_H
#define CVITE_RUNTIME_INTERNAL_H

#include "cvite/runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define CVITE_PRINTF_LIKE(FORMAT_INDEX, FIRST_ARGUMENT) \
    __attribute__((format(printf, FORMAT_INDEX, FIRST_ARGUMENT)))
#else
#define CVITE_PRINTF_LIKE(FORMAT_INDEX, FIRST_ARGUMENT)
#endif

typedef struct cvite_function_record {
    cvite_id id;
    cvite_id abi_fingerprint;
    char *debug_name;
    size_t slot;
} cvite_function_record;

typedef struct cvite_storage_record {
    cvite_id id;
    cvite_id layout_fingerprint;
    char *debug_name;
    void *address;
    size_t size;
    size_t alignment;
} cvite_storage_record;

typedef struct cvite_dispatch_snapshot {
    struct cvite_dispatch_snapshot *retired_next;
    uint64_t generation;
    size_t slot_count;
    cvite_function_pointer targets[];
} cvite_dispatch_snapshot;

struct cvite_runtime {
    atomic_flag writer_lock;
    atomic_bool snapshot_collection_gate;
    atomic_uint_fast64_t active_snapshot_readers;
    _Atomic(cvite_dispatch_snapshot *) active_snapshot;
    cvite_dispatch_snapshot *retired_snapshots;

    cvite_function_record *functions;
    size_t function_count;
    size_t function_capacity;

    cvite_storage_record *storage;
    size_t storage_count;
    size_t storage_capacity;

    bool sealed;
};

void cvite_internal_lock(cvite_runtime *runtime);
void cvite_internal_unlock(cvite_runtime *runtime);
void cvite_internal_snapshot_reader_enter(cvite_runtime *runtime);
void cvite_internal_snapshot_reader_leave(cvite_runtime *runtime);

CVITE_PRINTF_LIKE(6, 7)
cvite_status cvite_internal_fail(
    cvite_error *error,
    cvite_status status,
    cvite_id symbol,
    cvite_id expected,
    cvite_id actual,
    const char *format,
    ...);

char *cvite_internal_duplicate_string(const char *source);
bool cvite_internal_is_power_of_two(size_t value);
void *cvite_internal_allocate_aligned(size_t alignment, size_t size);
void cvite_internal_free_aligned(void *memory);

size_t cvite_internal_find_function_index(
    const cvite_runtime *runtime,
    cvite_id id);
size_t cvite_internal_find_storage_index(
    const cvite_runtime *runtime,
    cvite_id id);

cvite_dispatch_snapshot *cvite_internal_allocate_snapshot(size_t slot_count);
cvite_status cvite_internal_grow_functions(
    cvite_runtime *runtime,
    cvite_error *error);
cvite_status cvite_internal_grow_storage(
    cvite_runtime *runtime,
    cvite_error *error);

#endif
