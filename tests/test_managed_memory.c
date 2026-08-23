#include "cvite/managed_memory.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct old_node {
    struct old_node *next;
    int value;
} old_node;

typedef struct new_node {
    struct new_node *next;
    int value;
    int generation;
} new_node;

typedef struct old_value {
    int value;
} old_value;

typedef struct new_value {
    int value;
    int added;
} new_value;

typedef struct fail_context {
    cvite_managed_object reject;
} fail_context;

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "managed-memory test failure: %s\n", message);
    }
    return condition;
}

static int fail_selected_object(
    cvite_managed_object object,
    const void *old_data,
    size_t old_size,
    void *new_data,
    size_t new_size,
    void *context,
    char *message,
    size_t message_size)
{
    const fail_context *failure = (const fail_context *)context;
    if (object == failure->reject) {
        if (message != NULL && message_size > 0U) {
            (void)snprintf(message, message_size, "intentional rollback");
        }
        return 0;
    }
    memset(new_data, 0, new_size);
    memcpy(new_data, old_data, old_size < new_size ? old_size : new_size);
    return 1;
}

static int allocate_object(
    cvite_managed_domain *domain,
    cvite_id type,
    size_t size,
    size_t alignment,
    cvite_managed_object *object,
    void **address,
    cvite_managed_error *error)
{
    return cvite_managed_allocate(
               domain,
               type,
               size,
               alignment,
               object,
               address,
               error) == CVITE_MANAGED_OK;
}

static int test_owner_relative_cycle(
    cvite_managed_domain *domain,
    cvite_managed_error *error)
{
    const cvite_id node_type = {UINT64_C(1), UINT64_C(101)};
    cvite_managed_object first_object = CVITE_MANAGED_OBJECT_INVALID;
    cvite_managed_object second_object = CVITE_MANAGED_OBJECT_INVALID;
    old_node *first_initial = NULL;
    old_node *second_initial = NULL;
    void *first_raw = NULL;
    void *second_raw = NULL;

    if (!allocate_object(
            domain,
            node_type,
            sizeof(old_node),
            _Alignof(old_node),
            &first_object,
            &first_raw,
            error) ||
        !allocate_object(
            domain,
            node_type,
            sizeof(old_node),
            _Alignof(old_node),
            &second_object,
            &second_raw,
            error)) {
        fprintf(stderr, "%s\n", error->message);
        return 0;
    }
    first_initial = (old_node *)first_raw;
    second_initial = (old_node *)second_raw;
    first_initial->value = 11;
    second_initial->value = 22;

    old_node *first = NULL;
    old_node *second = NULL;
    unsigned char *invalid_slot =
        (unsigned char *)first_initial + sizeof(old_node) - 1U;
    if (!check(
            cvite_managed_track_pointer(
                domain,
                invalid_slot,
                second_object,
                0U,
                error) == CVITE_MANAGED_INVALID_ARGUMENT,
            "an owner-relative slot crossing the allocation boundary was accepted") ||
        !check(
            cvite_managed_pointer_count(domain) == 0U,
            "rejected pointer slot changed the pointer registry")) {
        return 0;
    }

    if (!check(
            cvite_managed_track_pointer(
                domain,
                &first,
                first_object,
                0U,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(
            cvite_managed_track_pointer(
                domain,
                &second,
                second_object,
                0U,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(
            cvite_managed_track_pointer(
                domain,
                &first_initial->next,
                second_object,
                0U,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(
            cvite_managed_track_pointer_in_object(
                domain,
                second_object,
                offsetof(old_node, next),
                first_object,
                0U,
                error) == CVITE_MANAGED_OK,
            error->message)) {
        return 0;
    }

    if (!check(first->next == second, "first cycle edge is incorrect") ||
        !check(second->next == first, "second cycle edge is incorrect") ||
        !check(cvite_managed_pointer_count(domain) == 4U, "pointer count mismatch")) {
        return 0;
    }

    cvite_managed_object located = CVITE_MANAGED_OBJECT_INVALID;
    size_t located_offset = (size_t)-1;
    if (!check(
            cvite_managed_locate(
                domain,
                &first->next,
                &located,
                &located_offset,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(located == first_object, "managed locate found wrong object") ||
        !check(
            located_offset == offsetof(old_node, next),
            "managed locate found wrong offset")) {
        return 0;
    }

    old_node *old_first = first;
    old_node *old_second = second;
    const cvite_managed_copy_operation copies[] = {
        {offsetof(old_node, next), offsetof(new_node, next), sizeof(void *)},
        {offsetof(old_node, value), offsetof(new_node, value), sizeof(int)},
    };
    const cvite_managed_byte_plan plan = {
        sizeof(old_node),
        sizeof(new_node),
        _Alignof(new_node),
        copies,
        sizeof(copies) / sizeof(copies[0]),
    };
    size_t migrated = 0U;
    if (!check(
            cvite_managed_migrate_type(
                domain,
                node_type,
                sizeof(new_node),
                _Alignof(new_node),
                cvite_managed_apply_byte_plan,
                (void *)&plan,
                &migrated,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(migrated == 2U, "unexpected migrated node count") ||
        !check((void *)first != (void *)old_first, "first node did not move") ||
        !check((void *)second != (void *)old_second, "second node did not move")) {
        return 0;
    }

    const new_node *first_new = (const new_node *)first;
    const new_node *second_new = (const new_node *)second;
    if (!check(first_new->value == 11, "first value was not preserved") ||
        !check(second_new->value == 22, "second value was not preserved") ||
        !check(first_new->generation == 0, "first tail was not zeroed") ||
        !check(second_new->generation == 0, "second tail was not zeroed") ||
        !check(
            first_new->next == (const new_node *)second,
            "first owner-relative slot was not rewritten") ||
        !check(
            second_new->next == (const new_node *)first,
            "second owner-relative slot was not rewritten")) {
        return 0;
    }

    void *first_before_rejected_migration = first;
    migrated = 99U;
    if (!check(
            cvite_managed_migrate_type(
                domain,
                node_type,
                sizeof(int),
                _Alignof(int),
                cvite_managed_apply_byte_plan,
                (void *)&plan,
                &migrated,
                error) == CVITE_MANAGED_INVALID_STATE,
            "shrinking through a tracked owner slot was not rejected") ||
        !check(migrated == 0U, "rejected migration reported progress") ||
        !check(
            (void *)first == first_before_rejected_migration,
            "rejected migration changed a live address")) {
        return 0;
    }

    if (!check(
            cvite_managed_free(domain, first_object, error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(first == NULL, "free did not clear external first pointer") ||
        !check(
            ((new_node *)second)->next == NULL,
            "free did not clear owner-relative incoming pointer") ||
        !check(
            cvite_managed_pointer_count(domain) == 1U,
            "free left stale pointer records")) {
        return 0;
    }
    if (!check(
            cvite_managed_free(domain, second_object, error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(second == NULL, "free did not clear external second pointer") ||
        !check(cvite_managed_pointer_count(domain) == 0U, "pointer records remain")) {
        return 0;
    }
    return 1;
}

static int test_rollback_and_escape(
    cvite_managed_domain *domain,
    cvite_managed_error *error)
{
    const cvite_id rollback_type = {UINT64_C(2), UINT64_C(202)};
    cvite_managed_object first_object = CVITE_MANAGED_OBJECT_INVALID;
    cvite_managed_object second_object = CVITE_MANAGED_OBJECT_INVALID;
    old_value *first = NULL;
    old_value *second = NULL;
    void *first_raw = NULL;
    void *second_raw = NULL;
    if (!allocate_object(
            domain,
            rollback_type,
            sizeof(old_value),
            _Alignof(old_value),
            &first_object,
            &first_raw,
            error) ||
        !allocate_object(
            domain,
            rollback_type,
            sizeof(old_value),
            _Alignof(old_value),
            &second_object,
            &second_raw,
            error)) {
        return 0;
    }
    first = (old_value *)first_raw;
    second = (old_value *)second_raw;
    first->value = 31;
    second->value = 32;
    void *first_before = first;
    void *second_before = second;
    fail_context failure = {second_object};
    size_t migrated = 0U;
    if (!check(
            cvite_managed_migrate_type(
                domain,
                rollback_type,
                sizeof(new_value),
                _Alignof(new_value),
                fail_selected_object,
                &failure,
                &migrated,
                error) == CVITE_MANAGED_MIGRATOR_FAILED,
            "migrator failure did not roll back") ||
        !check(migrated == 0U, "rolled-back migration reported progress")) {
        return 0;
    }
    void *first_after = NULL;
    void *second_after = NULL;
    if (!check(
            cvite_managed_address(
                domain,
                first_object,
                &first_after,
                NULL,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(
            cvite_managed_address(
                domain,
                second_object,
                &second_after,
                NULL,
                error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(first_after == first_before, "rollback published first object") ||
        !check(second_after == second_before, "rollback published second object") ||
        !check(((old_value *)first_after)->value == 31, "rollback changed first data") ||
        !check(((old_value *)second_after)->value == 32, "rollback changed second data")) {
        return 0;
    }

    const cvite_managed_copy_operation copy = {
        offsetof(old_value, value),
        offsetof(new_value, value),
        sizeof(int),
    };
    const cvite_managed_byte_plan plan = {
        sizeof(old_value),
        sizeof(new_value),
        _Alignof(new_value),
        &copy,
        1U,
    };
    if (!check(
            cvite_managed_mark_escaped(domain, first_object, error) ==
                CVITE_MANAGED_OK,
            error->message) ||
        !check(
            cvite_managed_migrate_type(
                domain,
                rollback_type,
                sizeof(new_value),
                _Alignof(new_value),
                cvite_managed_apply_byte_plan,
                (void *)&plan,
                &migrated,
                error) == CVITE_MANAGED_POINTER_ESCAPED,
            "escaped managed object was migrated") ||
        !check(migrated == 0U, "escaped migration reported progress")) {
        return 0;
    }

    if (!check(
            cvite_managed_free(domain, first_object, error) == CVITE_MANAGED_OK,
            error->message) ||
        !check(
            cvite_managed_free(domain, second_object, error) == CVITE_MANAGED_OK,
            error->message)) {
        return 0;
    }
    return 1;
}

int main(void)
{
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    cvite_managed_domain *domain = NULL;
    if (!check(
            cvite_managed_domain_create(&domain, &error) == CVITE_MANAGED_OK,
            error.message)) {
        return 1;
    }

    if (!test_owner_relative_cycle(domain, &error) ||
        !test_rollback_and_escape(domain, &error) ||
        !check(cvite_managed_object_count(domain) == 0U, "objects remain") ||
        !check(cvite_managed_pointer_count(domain) == 0U, "pointers remain")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    cvite_managed_domain_destroy(domain);
    printf("managed memory transaction checks passed\n");
    return 0;
}
