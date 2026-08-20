#ifndef CVITE_CANDIDATE_H
#define CVITE_CANDIDATE_H

#include <stdint.h>

#include "cvite/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal compiler/loader ABI. A candidate object emits exactly one manifest
 * with this symbol. Ordinary application source never includes this header.
 */
#define CVITE_CANDIDATE_MANIFEST_SCHEMA UINT64_C(1)
#define CVITE_CANDIDATE_MANIFEST_SYMBOL "__cvite_candidate_manifest"

typedef struct cvite_candidate_function {
    uint64_t id_high;
    uint64_t id_low;
    uint64_t abi_high;
    uint64_t abi_low;
    cvite_function_pointer target;
    const char *debug_name;
} cvite_candidate_function;

typedef struct cvite_candidate_manifest {
    uint64_t schema;
    uint64_t function_count;
    const cvite_candidate_function *functions;
} cvite_candidate_manifest;

#ifdef __cplusplus
}
#endif

#endif
