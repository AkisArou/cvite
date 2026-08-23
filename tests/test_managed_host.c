#include "cvite/host.h"
#include "cvite/managed_host.h"

#include <stddef.h>
#include <stdio.h>

typedef struct node_v1 {
    int value;
    struct node_v1 *next;
} node_v1;

typedef struct node_v2 {
    int value;
    struct node_v2 *next;
    int health;
} node_v2;

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(                                                     \
                stderr, "requirement failed at %s:%d: %s\n",                 \
                __FILE__, __LINE__, #condition);                               \
            cvite_host_managed_shutdown();                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void)
{
    cvite_managed_error error;
    node_v1 initial_a = {31, NULL};
    node_v1 initial_b = {47, NULL};
    node_v1 *old_a = NULL;
    node_v1 *old_b = NULL;
    cvite_managed_object_view view_a;
    cvite_managed_object_view view_b;
    const cvite_managed_copy_range copy = {
        offsetof(node_v1, value), offsetof(node_v2, value), sizeof(int)};
    const cvite_managed_object_migration objects[] = {
        {1U, 11U, 12U, sizeof(node_v2), _Alignof(node_v2), &copy, 1U},
        {2U, 11U, 12U, sizeof(node_v2), _Alignof(node_v2), &copy, 1U},
    };
    const cvite_managed_migration migration = {objects, 2U};

    _Static_assert(
        offsetof(node_v1, next) == offsetof(node_v2, next),
        "append-only node pointer offset changed");
    _Static_assert(
        _Alignof(node_v1) == _Alignof(node_v2),
        "append-only node alignment changed");

    cvite_host_managed_shutdown();
    REQUIRE(cvite_host_managed_register_object(
                &(cvite_managed_object_definition){
                    1U,
                    11U,
                    sizeof(initial_a),
                    _Alignof(node_v1),
                    CVITE_MANAGED_OBJECT_RELOCATABLE,
                    &initial_a,
                    "host-node-a",
                },
                (void **)&old_a,
                &error) == CVITE_MANAGED_STATUS_OK);
    REQUIRE(cvite_host_managed_register_object(
                &(cvite_managed_object_definition){
                    2U,
                    11U,
                    sizeof(initial_b),
                    _Alignof(node_v1),
                    CVITE_MANAGED_OBJECT_RELOCATABLE,
                    &initial_b,
                    "host-node-b",
                },
                (void **)&old_b,
                &error) == CVITE_MANAGED_STATUS_OK);
    REQUIRE(cvite_host_managed_track_pointer(
                &(cvite_managed_pointer_definition){
                    1U, offsetof(node_v1, next), 2U, 0U},
                &error) == CVITE_MANAGED_STATUS_OK);
    REQUIRE(cvite_host_managed_track_pointer(
                &(cvite_managed_pointer_definition){
                    2U, offsetof(node_v1, next), 1U, 0U},
                &error) == CVITE_MANAGED_STATUS_OK);

    __cvite_host_call_enter();
    REQUIRE(cvite_host_managed_migrate(&migration, &error) ==
            CVITE_MANAGED_STATUS_BUSY);
    REQUIRE(old_a->value == 31);
    REQUIRE(old_b->value == 47);
    REQUIRE(old_a->next == old_b);
    REQUIRE(old_b->next == old_a);
    __cvite_host_call_leave();

    REQUIRE(cvite_host_managed_migrate(&migration, &error) ==
            CVITE_MANAGED_STATUS_OK);
    REQUIRE(cvite_host_managed_find_object(1U, &view_a, &error) ==
            CVITE_MANAGED_STATUS_OK);
    REQUIRE(cvite_host_managed_find_object(2U, &view_b, &error) ==
            CVITE_MANAGED_STATUS_OK);
    REQUIRE(view_a.address != old_a);
    REQUIRE(view_b.address != old_b);
    REQUIRE(((node_v2 *)view_a.address)->value == 31);
    REQUIRE(((node_v2 *)view_b.address)->value == 47);
    REQUIRE(((node_v2 *)view_a.address)->health == 0);
    REQUIRE(((node_v2 *)view_b.address)->health == 0);
    REQUIRE(((node_v2 *)view_a.address)->next == (node_v2 *)view_b.address);
    REQUIRE(((node_v2 *)view_b.address)->next == (node_v2 *)view_a.address);
    REQUIRE(cvite_host_managed_object_count() == 2U);
    REQUIRE(cvite_host_managed_pointer_count() == 2U);

    cvite_host_managed_shutdown();
    return 0;
}
