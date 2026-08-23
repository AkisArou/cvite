#include "cvite/runtime.h"

#include <stdio.h>
#include <stdlib.h>

typedef int (*update_fn)(int *value);

static int update_v1(int *value)
{
    *value += 1;
    return *value;
}

static int update_v2(int *value)
{
    *value += 100;
    return *value;
}

int main(void)
{
    const cvite_id function_id = CVITE_ID(0x01U, 0x01U);
    const cvite_id abi = CVITE_ID(0x02U, 0x02U);
    cvite_runtime *runtime = NULL;
    cvite_function_definition definition;
    cvite_function_update update;
    cvite_patch patch;
    cvite_error error;
    size_t slot = 0U;
    int state = 41;
    update_fn active = NULL;

    if (cvite_runtime_create(&runtime, &error) != CVITE_STATUS_OK) {
        (void)fprintf(stderr, "%s\n", error.message);
        return EXIT_FAILURE;
    }

    definition.id = function_id;
    definition.abi_fingerprint = abi;
    definition.initial_target = (cvite_function_pointer)update_v1;
    definition.debug_name = "update";

    if (cvite_runtime_register_function(runtime, &definition, &slot, &error) !=
            CVITE_STATUS_OK ||
        cvite_runtime_seal(runtime, &error) != CVITE_STATUS_OK) {
        (void)fprintf(stderr, "%s\n", error.message);
        cvite_runtime_destroy(runtime);
        return EXIT_FAILURE;
    }

    active = (update_fn)cvite_runtime_target_at(runtime, slot);
    (void)printf("generation 0: %d\n", active(&state));

    update.id = function_id;
    update.abi_fingerprint = abi;
    update.target = (cvite_function_pointer)update_v2;
    patch.expected_generation = 0U;
    patch.candidate_generation = 1U;
    patch.functions = &update;
    patch.function_count = 1U;

    if (cvite_runtime_apply_patch(runtime, &patch, &error) != CVITE_STATUS_OK) {
        (void)fprintf(stderr, "%s\n", error.message);
        cvite_runtime_destroy(runtime);
        return EXIT_FAILURE;
    }

    active = (update_fn)cvite_runtime_target_at(runtime, slot);
    (void)printf("generation 1: %d\n", active(&state));
    (void)printf("same process state: %d\n", state);

    cvite_runtime_destroy(runtime);
    return EXIT_SUCCESS;
}
