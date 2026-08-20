#include "cvite/host.h"
#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>

typedef int (*update_fn)(void *state, int delta);

typedef struct counter_state {
    int value;
    int updates;
} counter_state;

extern int cvite_e2e_update(void *state, int delta);

int cvite_e2e_multiplier(int value)
{
    return value * 10;
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

int main(int argc, char **argv)
{
    cvite_orc_loader *loader = NULL;
    cvite_orc_generation generation = CVITE_ORC_GENERATION_INVALID;
    cvite_host_function function;
    cvite_patch patch;
    cvite_error error;
    counter_state state = {5, 0};
    update_fn stable_address = cvite_e2e_update;

    CHECK(argc == 2);
    CHECK(stable_address(&state, 2) == 7);
    CHECK(state.updates == 1);
    CHECK(cvite_host_generation() == 0U);

    CHECK(cvite_host_find_function("cvite_e2e_update", &function, &error) ==
        CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_define_function(
              loader,
              "cvite_e2e_multiplier",
              (cvite_function_pointer)cvite_e2e_multiplier,
              &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &generation, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_patch(
              loader,
              generation,
              0U,
              1U,
              &patch,
              &error) == CVITE_STATUS_OK);
    CHECK(patch.function_count == 1U);
    CHECK(cvite_id_equal(patch.functions[0].id, function.id));
    CHECK(cvite_id_equal(
        patch.functions[0].abi_fingerprint, function.abi_fingerprint));
    CHECK(patch.functions[0].target != NULL);
    CHECK(cvite_host_apply_patch(&patch, &error) == CVITE_STATUS_OK);

    CHECK(cvite_e2e_update == stable_address);
    CHECK(stable_address(&state, 3) == 37);
    CHECK(state.updates == 2);
    CHECK(cvite_host_generation() == 1U);

    /* Keep the published JIT generation alive until process exit. */
    (void)loader;
    return EXIT_SUCCESS;
}
