#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

cvite_status cvite_runtime_apply_patch(
    cvite_runtime *runtime,
    const cvite_patch *patch,
    cvite_error *error)
{
    cvite_status status = CVITE_STATUS_OK;
    cvite_dispatch_snapshot *old_snapshot = NULL;
    cvite_dispatch_snapshot *new_snapshot = NULL;
    size_t update_index = 0U;

    cvite_error_clear(error);
    if (runtime == NULL || patch == NULL || patch->functions == NULL ||
        patch->function_count == 0U ||
        patch->candidate_generation <= patch->expected_generation) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid patch descriptor");
    }

    cvite_internal_lock(runtime);
    if (!runtime->sealed) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "runtime must be sealed before applying patches");
        goto done;
    }

    old_snapshot = atomic_load_explicit(
        &runtime->active_snapshot, memory_order_acquire);
    if (patch->expected_generation != old_snapshot->generation) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_STALE_GENERATION,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "patch expected generation %llu but active generation is %llu",
            (unsigned long long)patch->expected_generation,
            (unsigned long long)old_snapshot->generation);
        goto done;
    }

    for (update_index = 0U; update_index < patch->function_count; ++update_index) {
        const cvite_function_update *update = &patch->functions[update_index];
        size_t previous_index = 0U;
        size_t function_index = SIZE_MAX;

        if (cvite_id_is_zero(update->id) ||
            cvite_id_is_zero(update->abi_fingerprint) ||
            update->target == NULL) {
            status = cvite_internal_fail(
                error,
                CVITE_STATUS_INVALID_ARGUMENT,
                update->id,
                CVITE_ID_ZERO,
                CVITE_ID_ZERO,
                "patch contains an invalid function update");
            goto done;
        }
        for (previous_index = 0U; previous_index < update_index; ++previous_index) {
            if (cvite_id_equal(
                    patch->functions[previous_index].id, update->id)) {
                status = cvite_internal_fail(
                    error,
                    CVITE_STATUS_DUPLICATE_SYMBOL,
                    update->id,
                    CVITE_ID_ZERO,
                    CVITE_ID_ZERO,
                    "patch updates the same function more than once");
                goto done;
            }
        }

        function_index = cvite_internal_find_function_index(runtime, update->id);
        if (function_index == SIZE_MAX) {
            status = cvite_internal_fail(
                error,
                CVITE_STATUS_UNKNOWN_SYMBOL,
                update->id,
                CVITE_ID_ZERO,
                CVITE_ID_ZERO,
                "patch references an unknown function");
            goto done;
        }
        if (!cvite_id_equal(
                runtime->functions[function_index].abi_fingerprint,
                update->abi_fingerprint)) {
            status = cvite_internal_fail(
                error,
                CVITE_STATUS_ABI_MISMATCH,
                update->id,
                runtime->functions[function_index].abi_fingerprint,
                update->abi_fingerprint,
                "function '%s' changed ABI; patch rejected",
                runtime->functions[function_index].debug_name);
            goto done;
        }
    }

    new_snapshot = cvite_internal_allocate_snapshot(old_snapshot->slot_count);
    if (new_snapshot == NULL) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "could not stage dispatch table");
        goto done;
    }

    new_snapshot->generation = patch->candidate_generation;
    memcpy(
        new_snapshot->targets,
        old_snapshot->targets,
        old_snapshot->slot_count * sizeof(new_snapshot->targets[0]));

    for (update_index = 0U; update_index < patch->function_count; ++update_index) {
        const cvite_function_update *update = &patch->functions[update_index];
        size_t function_index =
            cvite_internal_find_function_index(runtime, update->id);
        new_snapshot->targets[runtime->functions[function_index].slot] =
            update->target;
    }

    old_snapshot->retired_next = runtime->retired_snapshots;
    runtime->retired_snapshots = old_snapshot;
    atomic_store_explicit(
        &runtime->active_snapshot, new_snapshot, memory_order_release);
    new_snapshot = NULL;

done:
    free(new_snapshot);
    cvite_internal_unlock(runtime);
    return status;
}
