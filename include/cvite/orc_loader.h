#ifndef CVITE_ORC_LOADER_H
#define CVITE_ORC_LOADER_H

#include <stdint.h>

#include "cvite/baseline.h"
#include "cvite/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal host API. Ordinary C application source never includes it. */

typedef struct cvite_orc_loader cvite_orc_loader;
typedef uint64_t cvite_orc_generation;

#define CVITE_ORC_GENERATION_INVALID UINT64_C(0)

cvite_status cvite_orc_loader_create(
    cvite_orc_loader **loader,
    cvite_error *error);

void cvite_orc_loader_destroy(cvite_orc_loader *loader);

cvite_status cvite_orc_loader_define_function(
    cvite_orc_loader *loader,
    const char *name,
    cvite_function_pointer address,
    cvite_error *error);

cvite_status cvite_orc_loader_stage_object(
    cvite_orc_loader *loader,
    const char *object_path,
    cvite_orc_generation *generation,
    cvite_error *error);

cvite_status cvite_orc_loader_lookup_function(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    const char *symbol_name,
    cvite_function_pointer *address,
    cvite_error *error);

/* Register a JIT baseline manifest and return its canonical C main entry. */
cvite_status cvite_orc_loader_prepare_baseline(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    cvite_program_main *program_main,
    cvite_error *error);

/*
 * Resolve and validate the compiler-generated candidate manifest, then expose
 * a generation-owned patch view. patch->functions remains valid until the
 * generation is discarded, the same generation is prepared again, or the
 * loader is destroyed.
 */
cvite_status cvite_orc_loader_prepare_patch(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    uint64_t expected_generation,
    uint64_t candidate_generation,
    cvite_patch *patch,
    cvite_error *error);

cvite_status cvite_orc_loader_discard_generation(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    cvite_error *error);

#ifdef __cplusplus
}
#endif

#endif
