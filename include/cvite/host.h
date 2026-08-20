#ifndef CVITE_HOST_H
#define CVITE_HOST_H

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

cvite_status cvite_host_register_function(
    const cvite_function_definition *definition,
    cvite_host_function *function,
    cvite_error *error);

uint64_t __cvite_host_register_function(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t abi_high,
    uint64_t abi_low,
    cvite_function_pointer initial_target,
    const char *debug_name);

cvite_function_pointer __cvite_host_target_at(uint64_t slot);

cvite_function_pointer __cvite_host_target_for(
    uint64_t id_high,
    uint64_t id_low,
    uint64_t abi_high,
    uint64_t abi_low);

cvite_status cvite_host_find_function(
    const char *debug_name,
    cvite_host_function *function,
    cvite_error *error);

cvite_status cvite_host_apply_patch(
    const cvite_patch *patch,
    cvite_error *error);

uint64_t cvite_host_generation(void);

#ifdef __cplusplus
}
#endif

#endif
