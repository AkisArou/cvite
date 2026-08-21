#include "cvite/host.h"
#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>

typedef int (*update_fn)(void *state, int delta);

typedef struct counter_state {
    int value;
    int updates;
} counter_state;

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
    cvite_orc_generation baseline = CVITE_ORC_GENERATION_INVALID;
    cvite_orc_generation candidate = CVITE_ORC_GENERATION_INVALID;
    cvite_program_main program_main = NULL;
    cvite_function_pointer address = NULL;
    cvite_host_function function;
    cvite_patch patch;
    cvite_error error;
    counter_state state = {5, 0};
    update_fn stable_address = NULL;

    CHECK(argc == 3);
    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_define_function(
              loader,
              "cvite_e2e_multiplier",
              (cvite_function_pointer)cvite_e2e_multiplier,
              &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &baseline, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_lookup_function(
              loader,
              baseline,
              "cvite_e2e_update",
              &address,
              &error) == CVITE_STATUS_OK);
    stable_address = (update_fn)address;
    CHECK(stable_address != NULL);
    CHECK(cvite_orc_loader_prepare_baseline(
              loader, baseline, &program_main, &error) == CVITE_STATUS_OK);
    CHECK(program_main != NULL);

    CHECK(cvite_host_storage_count() == 1U);
    CHECK(stable_address(&state, 2) == 17);
    CHECK(state.updates == 1);
    CHECK(cvite_host_generation() == 0U);

    CHECK(cvite_host_find_function("cvite_e2e_update", &function, &error) ==
        CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[2], &candidate, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_patch(
              loader,
              candidate,
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
    CHECK(cvite_orc_loader_commit_patch(loader, candidate, &error) ==
        CVITE_STATUS_OK);

    CHECK(stable_address(&state, 3) == 58);
    CHECK(state.updates == 2);
    CHECK(cvite_host_generation() == 1U);

    cvite_orc_loader_destroy(loader);
    return EXIT_SUCCESS;
}
