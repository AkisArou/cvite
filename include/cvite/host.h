#ifndef CVITE_HOST_H
#define CVITE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cvite/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal host/runtime ABI. The compiler-generated baseline calls the
 * __cvite_host_* entry points; ordinary application source never includes this
 * header and never calls these functions.
 */

typedef struct cvite_host_function {
    cvite_id id;
    cvite_id abi_fingerprint;
    size_t slot;
    const char *debug_name;
} cvite_host_function;

typedef struct cvite_host_storage {
    cvite_id id;
    cvite_id layout_fingerprint;
    void *address;
    size_t size;
    size_t alignment;
    const char *debug_name;
} cvite_host_storage;

cvite_status cvite_host_register_function(
    const cvite_function_definition *definition,
    cvite_host_function *function,
    cvite_error *error);

cvite_status cvite_host_register_storage(
    const cvite_storage_definition *definition,
    void *address,
    cvite_host_storage *storage,
    cvite_error *error);

cvite_status cvite_host_require_storage(
    const cvite_storage_definition *definition,
    cvite_host_storage *storage,
    cvite_error *error);

uint64_t __cvite_host_register_function(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t abi_high,
    uint64_t abi_low,
    cvite_function_pointer initial_target,
    const char *debug_name);

void *__cvite_host_register_storage(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t layout_high,
    uint64_t layout_low,
    uint64_t size,
    uint64_t alignment,
    void *address,
    const char *debug_name);

void __cvite_host_call_enter(void);
void __cvite_host_call_leave(void);

cvite_function_pointer __cvite_host_target_at(uint64_t slot);

cvite_function_pointer __cvite_host_target_for(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t abi_high,
    uint64_t abi_low);

void *__cvite_host_storage_for(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t layout_high,
    uint64_t layout_low,
    uint64_t size,
    uint64_t alignment,
    const char *debug_name);

cvite_status cvite_host_find_function(
    const char *debug_name,
    cvite_host_function *function,
    cvite_error *error);

size_t cvite_host_storage_count(void);

cvite_status cvite_host_storage_at(
    size_t index,
    cvite_host_storage *storage,
    cvite_error *error);

cvite_status cvite_host_apply_patch(
    const cvite_patch *patch,
    cvite_error *error);

cvite_status cvite_host_collect_retired_snapshots(
    size_t *reclaimed_count,
    cvite_error *error);

uint64_t cvite_host_generation(void);

/* Internal stop-the-world gate used only while retired JIT code is removed. */
bool cvite_host_try_begin_quiescence(void);
void cvite_host_end_quiescence(void);
uint64_t cvite_host_active_call_count(void);

#ifdef __cplusplus
}
#endif

#endif
