#ifndef CVITE_BASELINE_H
#define CVITE_BASELINE_H

#include <stdint.h>

#include "cvite/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal compiler/loader ABI. Ordinary application source never includes it. */
#define CVITE_BASELINE_MANIFEST_SCHEMA UINT64_C(1)
#define CVITE_BASELINE_MANIFEST_SYMBOL "__cvite_baseline_manifest"
#define CVITE_PROGRAM_MAIN_SYMBOL "__cvite_program_main"

typedef struct cvite_baseline_function {
    uint64_t id_high;
    uint64_t id_low;
    uint64_t abi_high;
    uint64_t abi_low;
    cvite_function_pointer initial_target;
    uint64_t *slot;
    const char *debug_name;
} cvite_baseline_function;

typedef struct cvite_baseline_manifest {
    uint64_t schema;
    uint64_t function_count;
    const cvite_baseline_function *functions;
} cvite_baseline_manifest;

typedef int (*cvite_program_main)(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
