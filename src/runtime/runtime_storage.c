#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

cvite_status cvite_runtime_register_storage(
    cvite_runtime *runtime,
    const cvite_storage_definition *definition,
    void **storage,
    cvite_error *error)
{
    cvite_status status = CVITE_STATUS_OK;
    cvite_storage_record *record = NULL;
    size_t existing_index = SIZE_MAX;
    void *memory = NULL;
    char *name = NULL;

    cvite_error_clear(error);
    if (runtime == NULL || definition == NULL || storage == NULL ||
        cvite_id_is_zero(definition->id) ||
        cvite_id_is_zero(definition->layout_fingerprint) ||
        definition->size == 0U ||
        !cvite_internal_is_power_of_two(definition->alignment)) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            definition != NULL ? definition->id : CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid storage definition");
    }

    *storage = NULL;
    cvite_internal_lock(runtime);
    existing_index = cvite_internal_find_storage_index(runtime, definition->id);
    if (existing_index != SIZE_MAX) {
        record = &runtime->storage[existing_index];
        if (record->size != definition->size ||
            record->alignment != definition->alignment ||
            !cvite_id_equal(
                record->layout_fingerprint, definition->layout_fingerprint)) {
            status = cvite_internal_fail(
                error,
                CVITE_STATUS_LAYOUT_MISMATCH,
                definition->id,
                record->layout_fingerprint,
                definition->layout_fingerprint,
                "storage '%s' changed layout (size %zu -> %zu, alignment %zu -> %zu)",
                record->debug_name,
                record->size,
                definition->size,
                record->alignment,
                definition->alignment);
        } else {
            *storage = record->address;
        }
        goto done;
    }

    if (runtime->sealed) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            definition->id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "new persistent storage cannot be declared after sealing in M0");
        goto done;
    }
    if (runtime->storage_count == runtime->storage_capacity) {
        status = cvite_internal_grow_storage(runtime, error);
        if (status != CVITE_STATUS_OK) {
            goto done;
        }
    }

    memory = cvite_internal_allocate_aligned(
        definition->alignment, definition->size);
    name = cvite_internal_duplicate_string(definition->debug_name);
    if (memory == NULL || name == NULL) {
        status = cvite_internal_fail(
            error,
            CVITE_STATUS_OUT_OF_MEMORY,
            definition->id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "could not allocate persistent storage");
        goto done;
    }

    if (definition->initial_data != NULL) {
        memcpy(memory, definition->initial_data, definition->size);
    } else {
        memset(memory, 0, definition->size);
    }

    runtime->storage[runtime->storage_count] = (cvite_storage_record){
        definition->id,
        definition->layout_fingerprint,
        name,
        memory,
        definition->size,
        definition->alignment,
    };
    runtime->storage_count += 1U;
    *storage = memory;
    name = NULL;
    memory = NULL;

done:
    free(name);
    cvite_internal_free_aligned(memory);
    cvite_internal_unlock(runtime);
    return status;
}

cvite_status cvite_runtime_find_storage(
    const cvite_runtime *runtime,
    cvite_id id,
    void **storage,
    size_t *size,
    cvite_error *error)
{
    size_t index = SIZE_MAX;

    cvite_error_clear(error);
    if (runtime == NULL || storage == NULL || cvite_id_is_zero(id)) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "invalid storage lookup");
    }

    index = cvite_internal_find_storage_index(runtime, id);
    if (index == SIZE_MAX) {
        return cvite_internal_fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            id,
            CVITE_ID_ZERO,
            CVITE_ID_ZERO,
            "persistent storage is not registered");
    }

    *storage = runtime->storage[index].address;
    if (size != NULL) {
        *size = runtime->storage[index].size;
    }
    return CVITE_STATUS_OK;
}
