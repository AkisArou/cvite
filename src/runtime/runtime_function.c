#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

cvite_status cvite_runtime_register_function(
    cvite_runtime *runtime,
    const cvite_function_definition *definition,
    size_t *slot,
    cvite_error *error)
{
    cvite_status status = CVITE_STATUS_OK;
    cvite_dispatch_snapshot *old_snapshot = NULL;
    cvite_dispatch_snapshot *new_snapshot = NULL;
    char *name = NULL;
    size_t old_count = 0U;

    cvite_error_clear(error);
    if (runtime == NULL || definition == NULL || slot == NULL ||
        cvite_id_is_zero(definition->id) ||
        cvite_id_is_zero(definition->abi_fingerprint) ||
        definition->initial_target == NULL) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            definition != NULL ? definition->id : CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid function definition");
    }

    cvite_internal_lock(runtime);
    if (runtime->sealed) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            definition->id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "function declarations are closed after sealing");
        goto done;
    }
    if (cvite_internal_find_function_index(runtime, definition->id) != SIZE_MAX) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_DUPLICATE_SYMBOL,
            definition->id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "function '%s' is already registered",
            definition->debug_name != NULL ? definition->debug_name : "<unnamed>");
        goto done;
    }
    if (runtime->function_count == runtime->function_capacity) {
        status = cvite_internal_grow_functions(runtime, error);
        if (status != CVITE_STATUS_OK) {
            goto done;
        }
    }

    name = cvite_internal_duplicate_string(definition->debug_name);
    old_snapshot = atomic_load_explicit(
        &runtime->active_snapshot, memory_order_acquire);
    old_count = old_snapshot->slot_count;
    new_snapshot = cvite_internal_allocate_snapshot(old_count + 1U);
    if (name == NULL || new_snapshot == NULL) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            definition->id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "could not extend function registry");
        goto done;
    }

    new_snapshot->generation = old_snapshot->generation;
    if (old_count > 0U) {
        memcpy(
            new_snapshot->targets,
            old_snapshot->targets,
            old_count * sizeof(new_snapshot->targets[0]));
    }
    new_snapshot->targets[old_count] = definition->initial_target;

    runtime->functions[runtime->function_count] = (cvite_function_record){
        definition->id,
        definition->abi_fingerprint,
        name,
        old_count,
    };
    runtime->function_count += 1U;
    *slot = old_count;
    name = NULL;

    old_snapshot->retired_next = runtime->retired_snapshots;
    runtime->retired_snapshots = old_snapshot;
    atomic_store_explicit(
        &runtime->active_snapshot, new_snapshot, memory_order_release);
    new_snapshot = NULL;

done:
    free(name);
    free(new_snapshot);
    cvite_internal_unlock(runtime);
    return status;
}

cvite_status cvite_runtime_function_slot(
    const cvite_runtime *runtime,
    cvite_id id,
    size_t *slot,
    cvite_error *error)
{
    size_t index = SIZE_MAX;

    cvite_error_clear(error);
    if (runtime == NULL || slot == NULL || cvite_id_is_zero(id)) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid function-slot lookup");
    }

    index = cvite_internal_find_function_index(runtime, id);
    if (index == SIZE_MAX) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "function is not registered");
    }

    *slot = runtime->functions[index].slot;
    return CVITE_STATUS_OK;
}

cvite_function_pointer cvite_runtime_target_at(
    const cvite_runtime *runtime,
    size_t slot)
{
    cvite_dispatch_snapshot *snapshot = NULL;

    if (runtime == NULL) {
        return NULL;
    }
    snapshot = atomic_load_explicit(
        &runtime->active_snapshot, memory_order_acquire);
    return slot < snapshot->slot_count ? snapshot->targets[slot] : NULL;
}

cvite_status cvite_runtime_acquire_view(
    const cvite_runtime *runtime,
    cvite_dispatch_view *view,
    cvite_error *error)
{
    cvite_dispatch_snapshot *snapshot = NULL;

    cvite_error_clear(error);
    if (runtime == NULL || view == NULL) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid dispatch view request");
    }

    snapshot = atomic_load_explicit(
        &runtime->active_snapshot, memory_order_acquire);
    *view = (cvite_dispatch_view){
        snapshot,
        snapshot->generation,
        snapshot->slot_count,
    };
    return CVITE_STATUS_OK;
}

cvite_function_pointer cvite_dispatch_view_target(
    const cvite_dispatch_view *view,
    size_t slot)
{
    const cvite_dispatch_snapshot *snapshot = NULL;

    if (view == NULL || view->snapshot == NULL || slot >= view->slot_count) {
        return NULL;
    }
    snapshot = (const cvite_dispatch_snapshot *)view->snapshot;
    return snapshot->targets[slot];
}
