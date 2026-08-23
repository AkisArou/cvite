#ifndef CVITE_RUNTIME_H
#define CVITE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "cvite/id.h"
#include "cvite/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal ABI used by compiler-generated code and the CVite host. Ordinary
 * application source is not expected to include this header.
 */

typedef void (*cvite_function_pointer)(void);

typedef struct cvite_runtime cvite_runtime;

typedef struct cvite_error {
    cvite_status status;
    cvite_id symbol;
    cvite_id expected_fingerprint;
    cvite_id actual_fingerprint;
    char message[256];
} cvite_error;

typedef struct cvite_function_definition {
    cvite_id id;
    cvite_id abi_fingerprint;
    cvite_function_pointer initial_target;
    const char *debug_name;
} cvite_function_definition;

typedef struct cvite_function_update {
    cvite_id id;
    cvite_id abi_fingerprint;
    cvite_function_pointer target;
} cvite_function_update;

typedef struct cvite_patch {
    uint64_t expected_generation;
    uint64_t candidate_generation;
    const cvite_function_update *functions;
    size_t function_count;
} cvite_patch;

typedef struct cvite_storage_definition {
    cvite_id id;
    cvite_id layout_fingerprint;
    size_t size;
    size_t alignment;
    const void *initial_data;
    const char *debug_name;
} cvite_storage_definition;

typedef struct cvite_dispatch_view {
    const cvite_runtime *runtime;
    const void *snapshot;
    uint64_t generation;
    size_t slot_count;
} cvite_dispatch_view;

cvite_status cvite_runtime_create(cvite_runtime **runtime, cvite_error *error);
void cvite_runtime_destroy(cvite_runtime *runtime);

cvite_status cvite_runtime_register_function(
    cvite_runtime *runtime,
    const cvite_function_definition *definition,
    size_t *slot,
    cvite_error *error);

cvite_status cvite_runtime_register_storage(
    cvite_runtime *runtime,
    const cvite_storage_definition *definition,
    void **storage,
    cvite_error *error);

cvite_status cvite_runtime_find_storage(
    const cvite_runtime *runtime,
    cvite_id id,
    void **storage,
    size_t *size,
    cvite_error *error);

cvite_status cvite_runtime_seal(cvite_runtime *runtime, cvite_error *error);

cvite_status cvite_runtime_apply_patch(
    cvite_runtime *runtime,
    const cvite_patch *patch,
    cvite_error *error);

uint64_t cvite_runtime_generation(const cvite_runtime *runtime);
size_t cvite_runtime_function_count(const cvite_runtime *runtime);

cvite_status cvite_runtime_function_slot(
    const cvite_runtime *runtime,
    cvite_id id,
    size_t *slot,
    cvite_error *error);

cvite_function_pointer cvite_runtime_target_at(
    const cvite_runtime *runtime,
    size_t slot);

/*
 * A dispatch view pins its immutable snapshot until release. Views must not
 * be copied or used after cvite_runtime_release_view.
 */
cvite_status cvite_runtime_acquire_view(
    const cvite_runtime *runtime,
    cvite_dispatch_view *view,
    cvite_error *error);

cvite_function_pointer cvite_dispatch_view_target(
    const cvite_dispatch_view *view,
    size_t slot);

void cvite_runtime_release_view(cvite_dispatch_view *view);

/*
 * Non-blocking collection. If a view or short target lookup is active, the
 * call succeeds with reclaimed_count set to zero and may be retried later.
 */
cvite_status cvite_runtime_collect_retired(
    cvite_runtime *runtime,
    size_t *reclaimed_count,
    cvite_error *error);

void cvite_error_clear(cvite_error *error);

#ifdef __cplusplus
}
#endif

#endif
