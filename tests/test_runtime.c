#include "cvite/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*counter_update_fn)(void *state, int delta);
typedef int (*counter_read_fn)(const void *state);

typedef struct counter_state {
    int value;
    int updates;
} counter_state;

static const cvite_id update_id = CVITE_ID_INITIALIZER(
    0x9ac42e64e58b743dU, 0xe6ca82a3f333c127U);
static const cvite_id read_id = CVITE_ID_INITIALIZER(
    0xf46d9231825fd3adU, 0x10a113a2dfd42531U);
static const cvite_id counter_abi = CVITE_ID_INITIALIZER(
    0x3d82878f834b5982U, 0xb288ef13bfae36a4U);
static const cvite_id read_abi = CVITE_ID_INITIALIZER(
    0x9c58c9271958682aU, 0x8b9058684449d643U);
static const cvite_id bad_abi = CVITE_ID_INITIALIZER(
    0x1111111111111111U, 0x2222222222222222U);
static const cvite_id state_id = CVITE_ID_INITIALIZER(
    0x3ed9e9dbb726ccf5U, 0x04d98f49d8d24d68U);
static const cvite_id state_layout = CVITE_ID_INITIALIZER(
    0xf0dd4cc7cb97ad20U, 0x6aa10c13128bd36dU);

static int update_v1(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;
    state->value += delta;
    state->updates += 1;
    return state->value;
}

static int update_v2(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;
    state->value += delta * 10;
    state->updates += 1;
    return state->value;
}

static int read_v1(const void *opaque)
{
    const counter_state *state = (const counter_state *)opaque;
    return state->value;
}

#define CHECK(CONDITION)                                                        \
    do {                                                                        \
        if (!(CONDITION)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                        \
                __FILE__, __LINE__, #CONDITION);                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static cvite_function_pointer erase_update(counter_update_fn function)
{
    return (cvite_function_pointer)function;
}

static cvite_function_pointer erase_read(counter_read_fn function)
{
    return (cvite_function_pointer)function;
}

static counter_update_fn restore_update(cvite_function_pointer function)
{
    return (counter_update_fn)function;
}

static counter_read_fn restore_read(cvite_function_pointer function)
{
    return (counter_read_fn)function;
}

int main(void)
{
    cvite_runtime *runtime = NULL;
    cvite_error error;
    cvite_status status = CVITE_STATUS_OK;
    cvite_function_definition update_definition;
    cvite_function_definition read_definition;
    cvite_storage_definition storage_definition;
    cvite_storage_definition incompatible_storage;
    counter_state initial_state = {7, 0};
    counter_state second_initializer = {999, 999};
    counter_state *state = NULL;
    void *same_storage = NULL;
    size_t update_slot = SIZE_MAX;
    size_t read_slot = SIZE_MAX;
    counter_update_fn update = NULL;
    counter_read_fn read = NULL;
    cvite_function_update valid_update;
    cvite_patch valid_patch;
    cvite_function_update mixed_updates[2];
    cvite_patch mixed_patch;

    status = cvite_runtime_create(&runtime, &error);
    CHECK(status == CVITE_STATUS_OK);
    CHECK(runtime != NULL);

    memset(&storage_definition, 0, sizeof(storage_definition));
    storage_definition.id = state_id;
    storage_definition.layout_fingerprint = state_layout;
    storage_definition.size = sizeof(initial_state);
    storage_definition.alignment = _Alignof(counter_state);
    storage_definition.initial_data = &initial_state;
    storage_definition.debug_name = "counter_state";

    status = cvite_runtime_register_storage(
        runtime, &storage_definition, (void **)&state, &error);
    CHECK(status == CVITE_STATUS_OK);
    CHECK(state != NULL);
    CHECK(state->value == 7);

    storage_definition.initial_data = &second_initializer;
    status = cvite_runtime_register_storage(
        runtime, &storage_definition, &same_storage, &error);
    CHECK(status == CVITE_STATUS_OK);
    CHECK(same_storage == state);
    CHECK(state->value == 7);
    CHECK(state->updates == 0);

    incompatible_storage = storage_definition;
    incompatible_storage.size += sizeof(int);
    status = cvite_runtime_register_storage(
        runtime, &incompatible_storage, &same_storage, &error);
    CHECK(status == CVITE_STATUS_LAYOUT_MISMATCH);
    CHECK(error.status == CVITE_STATUS_LAYOUT_MISMATCH);

    memset(&update_definition, 0, sizeof(update_definition));
    update_definition.id = update_id;
    update_definition.abi_fingerprint = counter_abi;
    update_definition.initial_target = erase_update(update_v1);
    update_definition.debug_name = "counter_update";

    status = cvite_runtime_register_function(
        runtime, &update_definition, &update_slot, &error);
    CHECK(status == CVITE_STATUS_OK);

    memset(&read_definition, 0, sizeof(read_definition));
    read_definition.id = read_id;
    read_definition.abi_fingerprint = read_abi;
    read_definition.initial_target = erase_read(read_v1);
    read_definition.debug_name = "counter_read";

    status = cvite_runtime_register_function(
        runtime, &read_definition, &read_slot, &error);
    CHECK(status == CVITE_STATUS_OK);
    CHECK(update_slot != read_slot);

    status = cvite_runtime_seal(runtime, &error);
    CHECK(status == CVITE_STATUS_OK);

    update = restore_update(cvite_runtime_target_at(runtime, update_slot));
    read = restore_read(cvite_runtime_target_at(runtime, read_slot));
    CHECK(update != NULL);
    CHECK(read != NULL);
    CHECK(update(state, 3) == 10);
    CHECK(read(state) == 10);
    CHECK(state->updates == 1);

    memset(&valid_update, 0, sizeof(valid_update));
    valid_update.id = update_id;
    valid_update.abi_fingerprint = counter_abi;
    valid_update.target = erase_update(update_v2);

    valid_patch.expected_generation = 0U;
    valid_patch.candidate_generation = 1U;
    valid_patch.functions = &valid_update;
    valid_patch.function_count = 1U;

    status = cvite_runtime_apply_patch(runtime, &valid_patch, &error);
    CHECK(status == CVITE_STATUS_OK);
    CHECK(cvite_runtime_generation(runtime) == 1U);

    update = restore_update(cvite_runtime_target_at(runtime, update_slot));
    CHECK(update(state, 2) == 30);
    CHECK(read(state) == 30);
    CHECK(state->updates == 2);

    status = cvite_runtime_apply_patch(runtime, &valid_patch, &error);
    CHECK(status == CVITE_STATUS_STALE_GENERATION);
    CHECK(cvite_runtime_generation(runtime) == 1U);

    mixed_updates[0] = valid_update;
    mixed_updates[0].target = erase_update(update_v1);
    mixed_updates[1].id = read_id;
    mixed_updates[1].abi_fingerprint = bad_abi;
    mixed_updates[1].target = erase_read(read_v1);

    mixed_patch.expected_generation = 1U;
    mixed_patch.candidate_generation = 2U;
    mixed_patch.functions = mixed_updates;
    mixed_patch.function_count = 2U;

    status = cvite_runtime_apply_patch(runtime, &mixed_patch, &error);
    CHECK(status == CVITE_STATUS_ABI_MISMATCH);
    CHECK(cvite_runtime_generation(runtime) == 1U);

    update = restore_update(cvite_runtime_target_at(runtime, update_slot));
    CHECK(update(state, 1) == 40);
    CHECK(state->updates == 3);

    cvite_runtime_destroy(runtime);
    return 0;
}
