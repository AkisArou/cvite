#include "cvite/host.h"

#include <stdio.h>
#include <stdlib.h>

typedef int (*update_fn)(int *state, int delta);

static int update_v1(int *state, int delta)
{
    *state += delta;
    return *state;
}

static int update_v2(int *state, int delta)
{
    *state += delta * 10;
    return *state;
}

static cvite_function_pointer erase(update_fn function)
{
    return (cvite_function_pointer)function;
}

static update_fn restore(cvite_function_pointer function)
{
    return (update_fn)function;
}

#define CHECK(CONDITION)                                                        \
    do {                                                                        \
        if (!(CONDITION)) {                                                     \
            (void)fprintf(                                                      \
                stderr,                                                         \
                "CHECK failed at %s:%d: %s\n",                                \
                __FILE__,                                                       \
                __LINE__,                                                       \
                #CONDITION);                                                    \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

int main(void)
{
    const cvite_id function_id = CVITE_ID(0x100U, 0x200U);
    const cvite_id abi = CVITE_ID(0x300U, 0x400U);
    cvite_host_function function;
    cvite_function_update update;
    cvite_patch patch;
    cvite_error error;
    uint64_t slot = 0U;
    int state = 5;
    update_fn active = NULL;

    slot = __cvite_host_register_function(
        function_id.high,
        function_id.low,
        abi.high,
        abi.low,
        erase(update_v1),
        "host_test_update");

    CHECK(cvite_host_find_function("host_test_update", &function, &error) ==
        CVITE_STATUS_OK);
    CHECK(function.slot == (size_t)slot);
    CHECK(cvite_id_equal(function.id, function_id));
    CHECK(cvite_id_equal(function.abi_fingerprint, abi));

    active = restore(__cvite_host_target_at(slot));
    CHECK(active(&state, 2) == 7);
    CHECK(cvite_host_generation() == 0U);

    update.id = function.id;
    update.abi_fingerprint = function.abi_fingerprint;
    update.target = erase(update_v2);
    patch.expected_generation = 0U;
    patch.candidate_generation = 1U;
    patch.functions = &update;
    patch.function_count = 1U;

    CHECK(cvite_host_apply_patch(&patch, &error) == CVITE_STATUS_OK);
    CHECK(cvite_host_generation() == 1U);

    active = restore(__cvite_host_target_at(slot));
    CHECK(active(&state, 3) == 37);
    return EXIT_SUCCESS;
}
