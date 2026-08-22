#ifndef CVITE_MANAGED_MEMORY_H
#define CVITE_MANAGED_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "cvite/id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal development-runtime ABI. Ordinary C source never calls this API. */

typedef uint64_t cvite_managed_object;
#define CVITE_MANAGED_OBJECT_INVALID UINT64_C(0)

typedef enum cvite_managed_status {
    CVITE_MANAGED_OK = 0,
    CVITE_MANAGED_INVALID_ARGUMENT = 1,
    CVITE_MANAGED_OUT_OF_MEMORY = 2,
    CVITE_MANAGED_NOT_FOUND = 3,
    CVITE_MANAGED_POINTER_ESCAPED = 4,
    CVITE_MANAGED_MIGRATOR_FAILED = 5,
    CVITE_MANAGED_INVALID_STATE = 6
} cvite_managed_status;

typedef struct cvite_managed_error {
    cvite_managed_status status;
    cvite_managed_object object;
    char message[256];
} cvite_managed_error;

typedef struct cvite_managed_domain cvite_managed_domain;

typedef struct cvite_managed_copy_operation {
    size_t old_offset;
    size_t new_offset;
    size_t size;
} cvite_managed_copy_operation;

typedef struct cvite_managed_byte_plan {
    size_t old_size;
    size_t new_size;
    size_t alignment;
    const cvite_managed_copy_operation *copies;
    size_t copy_count;
} cvite_managed_byte_plan;

typedef int (*cvite_managed_migrator)(
    cvite_managed_object object,
    const void *old_data,
    size_t old_size,
    void *new_data,
    size_t new_size,
    void *context,
    char *message,
    size_t message_size);

cvite_managed_status cvite_managed_domain_create(
    cvite_managed_domain **domain,
    cvite_managed_error *error);
void cvite_managed_domain_destroy(cvite_managed_domain *domain);

cvite_managed_status cvite_managed_allocate(
    cvite_managed_domain *domain,
    cvite_id type,
    size_t size,
    size_t alignment,
    cvite_managed_object *object,
    void **address,
    cvite_managed_error *error);

cvite_managed_status cvite_managed_free(
    cvite_managed_domain *domain,
    cvite_managed_object object,
    cvite_managed_error *error);

cvite_managed_status cvite_managed_address(
    cvite_managed_domain *domain,
    cvite_managed_object object,
    void **address,
    size_t *size,
    cvite_managed_error *error);

/* Resolve an address inside a managed object. One-past addresses are valid. */
cvite_managed_status cvite_managed_locate(
    cvite_managed_domain *domain,
    const void *address,
    cvite_managed_object *object,
    size_t *offset,
    cvite_managed_error *error);

/*
 * Track a pointer slot. If the slot itself resides in a managed object, its
 * owner object and byte offset are recorded instead of the absolute address.
 */
cvite_managed_status cvite_managed_track_pointer(
    cvite_managed_domain *domain,
    void *slot,
    cvite_managed_object target,
    size_t target_offset,
    cvite_managed_error *error);

cvite_managed_status cvite_managed_track_pointer_in_object(
    cvite_managed_domain *domain,
    cvite_managed_object owner,
    size_t owner_offset,
    cvite_managed_object target,
    size_t target_offset,
    cvite_managed_error *error);

cvite_managed_status cvite_managed_untrack_pointer(
    cvite_managed_domain *domain,
    void *slot,
    cvite_managed_error *error);

cvite_managed_status cvite_managed_mark_escaped(
    cvite_managed_domain *domain,
    cvite_managed_object object,
    cvite_managed_error *error);

/*
 * The caller must hold CVite's process-wide quiescence gate. All replacement
 * allocations and pointer writes are staged before any live address changes.
 */
cvite_managed_status cvite_managed_migrate_type(
    cvite_managed_domain *domain,
    cvite_id type,
    size_t new_size,
    size_t new_alignment,
    cvite_managed_migrator migrator,
    void *context,
    size_t *migrated_count,
    cvite_managed_error *error);

int cvite_managed_apply_byte_plan(
    cvite_managed_object object,
    const void *old_data,
    size_t old_size,
    void *new_data,
    size_t new_size,
    void *context,
    char *message,
    size_t message_size);

size_t cvite_managed_object_count(cvite_managed_domain *domain);
size_t cvite_managed_pointer_count(cvite_managed_domain *domain);
void cvite_managed_error_clear(cvite_managed_error *error);
const char *cvite_managed_status_name(cvite_managed_status status);

#ifdef __cplusplus
}
#endif

#endif
