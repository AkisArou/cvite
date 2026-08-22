#include "cvite/managed_memory.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct old_player {
    int score;
    float speed;
} old_player;

typedef struct new_player {
    int score;
    float speed;
    int health;
} new_player;

typedef struct fail_context {
    cvite_managed_object reject;
} fail_context;

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
    fail_context *failure = (fail_context *)context;
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

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "managed-memory test failure: %s\n", message);
    }
    return condition;
}

static int allocate_player(
    cvite_managed_domain *domain,
    cvite_id type,
    int score,
    float speed,
    cvite_managed_object *object,
    old_player **player,
    cvite_managed_error *error)
{
    void *address = NULL;
    if (cvite_managed_allocate(
            domain,
            type,
            sizeof(old_player),
            _Alignof(old_player),
            object,
            &address,
            error) != CVITE_MANAGED_OK) {
        return 0;
    }
    *player = (old_player *)address;
    (*player)->score = score;
    (*player)->speed = speed;
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

    const cvite_id player_type = {UINT64_C(1), UINT64_C(11)};
    cvite_managed_object first = CVITE_MANAGED_OBJECT_INVALID;
    cvite_managed_object second = CVITE_MANAGED_OBJECT_INVALID;
    old_player *first_initial = NULL;
    old_player *second_initial = NULL;
    if (!allocate_player(
            domain, player_type, 7, 1.5F, &first, &first_initial, &error) ||
        !allocate_player(
            domain, player_type, 9, 2.5F, &second, &second_initial, &error)) {
        fprintf(stderr, "%s\n", error.message);
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    void *first_pointer = NULL;
    void *first_speed_pointer = NULL;
    void *second_pointer = NULL;
    if (!check(
            cvite_managed_track_pointer(
                domain, &first_pointer, first, 0U, &error) == CVITE_MANAGED_OK,
            error.message) ||
        !check(
            cvite_managed_track_pointer(
                domain,
                &first_speed_pointer,
                first,
                offsetof(old_player, speed),
                &error) == CVITE_MANAGED_OK,
            error.message) ||
        !check(
            cvite_managed_track_pointer(
                domain, &second_pointer, second, 0U, &error) == CVITE_MANAGED_OK,
            error.message)) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    const void *old_first_address = first_pointer;
    const void *old_speed_address = first_speed_pointer;
    const cvite_managed_copy_operation copies[] = {
        {offsetof(old_player, score), offsetof(new_player, score), sizeof(int)},
        {offsetof(old_player, speed), offsetof(new_player, speed), sizeof(float)},
    };
    const cvite_managed_byte_plan plan = {
        sizeof(old_player),
        sizeof(new_player),
        _Alignof(new_player),
        copies,
        sizeof(copies) / sizeof(copies[0]),
    };
    size_t migrated = 0U;
    if (!check(
            cvite_managed_migrate_type(
                domain,
                player_type,
                sizeof(new_player),
                _Alignof(new_player),
                cvite_managed_apply_byte_plan,
                (void *)&plan,
                &migrated,
                &error) == CVITE_MANAGED_OK,
            error.message) ||
        !check(migrated == 2U, "unexpected migrated object count") ||
        !check(first_pointer != old_first_address, "direct pointer was not rewritten") ||
        !check(
            first_speed_pointer != old_speed_address,
            "interior pointer was not rewritten")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    const new_player *first_value = (const new_player *)first_pointer;
    const new_player *second_value = (const new_player *)second_pointer;
    if (!check(first_value->score == 7, "first score was not preserved") ||
        !check(first_value->speed == 1.5F, "first speed was not preserved") ||
        !check(first_value->health == 0, "first health was not initialized") ||
        !check(second_value->score == 9, "second score was not preserved") ||
        !check(second_value->health == 0, "second health was not initialized") ||
        !check(
            first_speed_pointer == (const void *)&first_value->speed,
            "interior pointer offset is incorrect")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    const cvite_id escaped_type = {UINT64_C(2), UINT64_C(22)};
    cvite_managed_object escaped = CVITE_MANAGED_OBJECT_INVALID;
    old_player *escaped_value = NULL;
    if (!allocate_player(
            domain, escaped_type, 4, 3.0F, &escaped, &escaped_value, &error) ||
        !check(
            cvite_managed_mark_escaped(domain, escaped, &error) ==
                CVITE_MANAGED_OK,
            error.message)) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    void *escaped_before = escaped_value;
    migrated = 99U;
    if (!check(
            cvite_managed_migrate_type(
                domain,
                escaped_type,
                sizeof(new_player),
                _Alignof(new_player),
                cvite_managed_apply_byte_plan,
                (void *)&plan,
                &migrated,
                &error) == CVITE_MANAGED_POINTER_ESCAPED,
            "escaped object migration was not rejected") ||
        !check(migrated == 0U, "rejected migration reported migrated objects")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    void *escaped_after = NULL;
    if (!check(
            cvite_managed_address(
                domain, escaped, &escaped_after, NULL, &error) ==
                CVITE_MANAGED_OK,
            error.message) ||
        !check(escaped_after == escaped_before, "escaped object moved on rejection")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    const cvite_id rollback_type = {UINT64_C(3), UINT64_C(33)};
    cvite_managed_object rollback_first = CVITE_MANAGED_OBJECT_INVALID;
    cvite_managed_object rollback_second = CVITE_MANAGED_OBJECT_INVALID;
    old_player *rollback_first_value = NULL;
    old_player *rollback_second_value = NULL;
    if (!allocate_player(
            domain,
            rollback_type,
            21,
            1.0F,
            &rollback_first,
            &rollback_first_value,
            &error) ||
        !allocate_player(
            domain,
            rollback_type,
            22,
            2.0F,
            &rollback_second,
            &rollback_second_value,
            &error)) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    void *rollback_first_before = rollback_first_value;
    void *rollback_second_before = rollback_second_value;
    fail_context failure = {rollback_second};
    if (!check(
            cvite_managed_migrate_type(
                domain,
                rollback_type,
                sizeof(new_player),
                _Alignof(new_player),
                fail_selected_object,
                &failure,
                &migrated,
                &error) == CVITE_MANAGED_MIGRATOR_FAILED,
            "migrator failure did not roll back")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    void *rollback_first_after = NULL;
    void *rollback_second_after = NULL;
    (void)cvite_managed_address(
        domain, rollback_first, &rollback_first_after, NULL, &error);
    (void)cvite_managed_address(
        domain, rollback_second, &rollback_second_after, NULL, &error);
    if (!check(
            rollback_first_after == rollback_first_before,
            "first rollback object was published") ||
        !check(
            rollback_second_after == rollback_second_before,
            "second rollback object was published")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    if (!check(cvite_managed_object_count(domain) == 5U, "object count mismatch")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    (void)cvite_managed_free(domain, first, &error);
    (void)cvite_managed_free(domain, second, &error);
    (void)cvite_managed_free(domain, escaped, &error);
    (void)cvite_managed_free(domain, rollback_first, &error);
    (void)cvite_managed_free(domain, rollback_second, &error);
    if (!check(first_pointer == NULL, "free did not clear tracked direct pointer") ||
        !check(
            first_speed_pointer == NULL,
            "free did not clear tracked interior pointer") ||
        !check(second_pointer == NULL, "free did not clear second pointer") ||
        !check(cvite_managed_object_count(domain) == 0U, "objects remain after free")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    cvite_managed_domain_destroy(domain);
    printf("managed memory transaction checks passed\n");
    return 0;
}
