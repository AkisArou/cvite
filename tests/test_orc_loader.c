#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>

typedef int (*update_fn)(void *state, int delta);

typedef struct counter_state {
    int value;
    int updates;
} counter_state;

static const cvite_id update_id = CVITE_ID_INITIALIZER(
    0x129147b459741c6eU, 0x46a4cbf1f5cd8e0cU);
static const cvite_id update_abi = CVITE_ID_INITIALIZER(
    0x1bdaab55d2047852U, 0x39ba639f460ca8a7U);

static int update_v1(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;
    state->value += delta;
    state->updates += 1;
    return state->value;
}

int cvite_host_multiplier(int value)
{
    return value * 10;
}

static cvite_function_pointer erase_update(update_fn function)
{
    return (cvite_function_pointer)function;
}

static update_fn restore_update(cvite_function_pointer function)
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

int main(int argc, char **argv)
{
    cvite_runtime *runtime = NULL;
    cvite_orc_loader *loader = NULL;
    cvite_error error;
    cvite_function_definition definition;
    cvite_function_update update;
    cvite_patch patch;
    cvite_orc_generation broken_generation = CVITE_ORC_GENERATION_INVALID;
    cvite_orc_generation valid_generation = CVITE_ORC_GENERATION_INVALID;
    cvite_function_pointer candidate = NULL;
    counter_state state = {5, 0};
    size_t slot = SIZE_MAX;
    update_fn active = NULL;

    CHECK(argc == 3);
    CHECK(cvite_runtime_create(&runtime, &error) == CVITE_STATUS_OK);

    definition.id = update_id;
    definition.abi_fingerprint = update_abi;
    definition.initial_target = erase_update(update_v1);
    definition.debug_name = "counter_update";
    CHECK(cvite_runtime_register_function(runtime, &definition, &slot, &error) ==
        CVITE_STATUS_OK);
    CHECK(cvite_runtime_seal(runtime, &error) == CVITE_STATUS_OK);

    active = restore_update(cvite_runtime_target_at(runtime, slot));
    CHECK(active(&state, 2) == 7);
    CHECK(state.updates == 1);

    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_define_function(
              loader,
              "cvite_host_multiplier",
              (cvite_function_pointer)cvite_host_multiplier,
              &error) == CVITE_STATUS_OK);

    CHECK(cvite_orc_loader_stage_object(
              loader, argv[2], &broken_generation, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_lookup_function(
              loader,
              broken_generation,
              "__cvite_broken_update_g1",
              &candidate,
              &error) == CVITE_STATUS_LINK_ERROR);
    CHECK(candidate == NULL);
    CHECK(cvite_runtime_generation(runtime) == 0U);
    CHECK(cvite_orc_loader_discard_generation(
              loader, broken_generation, &error) == CVITE_STATUS_OK);

    active = restore_update(cvite_runtime_target_at(runtime, slot));
    CHECK(active(&state, 1) == 8);
    CHECK(state.updates == 2);

    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &valid_generation, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_lookup_function(
              loader,
              valid_generation,
              "__cvite_candidate_update_g1",
              &candidate,
              &error) == CVITE_STATUS_OK);
    CHECK(candidate != NULL);

    update.id = update_id;
    update.abi_fingerprint = update_abi;
    update.target = candidate;
    patch.expected_generation = 0U;
    patch.candidate_generation = 1U;
    patch.functions = &update;
    patch.function_count = 1U;
    CHECK(cvite_runtime_apply_patch(runtime, &patch, &error) == CVITE_STATUS_OK);

    active = restore_update(cvite_runtime_target_at(runtime, slot));
    CHECK(active(&state, 3) == 38);
    CHECK(state.updates == 3);
    CHECK(cvite_runtime_generation(runtime) == 1U);

    /* The runtime must stop referencing JIT code before its generation is freed. */
    cvite_runtime_destroy(runtime);
    cvite_orc_loader_destroy(loader);
    return EXIT_SUCCESS;
}
