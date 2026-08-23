#include "cvite/runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*value_fn)(void);

typedef struct test_context {
    cvite_runtime *runtime;
    size_t first_slot;
    size_t second_slot;
    atomic_bool stop;
    atomic_int failures;
} test_context;

static const cvite_id first_id = CVITE_ID_INITIALIZER(
    0xa741c67d41db244cU, 0x4f7edfbf0b37d69dU);
static const cvite_id second_id = CVITE_ID_INITIALIZER(
    0x6ef9b5ab0c590f65U, 0xb5aef67f2ca76813U);
static const cvite_id value_abi = CVITE_ID_INITIALIZER(
    0x96060da6513a660fU, 0xd72d9d7566909be7U);

static int first_v1(void) { return 10; }
static int second_v1(void) { return 20; }
static int first_v2(void) { return 100; }
static int second_v2(void) { return 200; }

static cvite_function_pointer erase(value_fn function)
{
    return (cvite_function_pointer)function;
}

static value_fn restore(cvite_function_pointer function)
{
    return (value_fn)function;
}

static void *reader_main(void *opaque)
{
    test_context *context = (test_context *)opaque;

    while (!atomic_load_explicit(&context->stop, memory_order_relaxed)) {
        cvite_dispatch_view view;
        value_fn first = NULL;
        value_fn second = NULL;
        int sum = 0;

        if (cvite_runtime_acquire_view(context->runtime, &view, NULL) !=
            CVITE_STATUS_OK) {
            atomic_fetch_add_explicit(&context->failures, 1, memory_order_relaxed);
            continue;
        }

        first = restore(cvite_dispatch_view_target(&view, context->first_slot));
        second = restore(cvite_dispatch_view_target(&view, context->second_slot));
        if (first == NULL || second == NULL) {
            atomic_fetch_add_explicit(&context->failures, 1, memory_order_relaxed);
            cvite_runtime_release_view(&view);
            continue;
        }

        sum = first() + second();
        if (sum != 30 && sum != 300) {
            atomic_fetch_add_explicit(&context->failures, 1, memory_order_relaxed);
        }
        cvite_runtime_release_view(&view);
    }

    return NULL;
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

int main(void)
{
    enum { reader_count = 4, patch_count = 20000 };
    cvite_runtime *runtime = NULL;
    cvite_error error;
    cvite_function_definition first_definition;
    cvite_function_definition second_definition;
    cvite_function_update updates[2];
    cvite_patch patch;
    pthread_t readers[reader_count];
    test_context context;
    uint64_t generation = 0U;
    size_t reclaimed = 0U;
    size_t total_reclaimed = 0U;
    int index = 0;

    CHECK(cvite_runtime_create(&runtime, &error) == CVITE_STATUS_OK);

    first_definition.id = first_id;
    first_definition.abi_fingerprint = value_abi;
    first_definition.initial_target = erase(first_v1);
    first_definition.debug_name = "first";
    CHECK(cvite_runtime_register_function(
        runtime, &first_definition, &context.first_slot, &error) == CVITE_STATUS_OK);

    second_definition.id = second_id;
    second_definition.abi_fingerprint = value_abi;
    second_definition.initial_target = erase(second_v1);
    second_definition.debug_name = "second";
    CHECK(cvite_runtime_register_function(
        runtime, &second_definition, &context.second_slot, &error) == CVITE_STATUS_OK);
    CHECK(cvite_runtime_seal(runtime, &error) == CVITE_STATUS_OK);

    context.runtime = runtime;
    atomic_init(&context.stop, false);
    atomic_init(&context.failures, 0);

    for (index = 0; index < reader_count; ++index) {
        CHECK(pthread_create(&readers[index], NULL, reader_main, &context) == 0);
    }

    updates[0].id = first_id;
    updates[0].abi_fingerprint = value_abi;
    updates[1].id = second_id;
    updates[1].abi_fingerprint = value_abi;
    patch.functions = updates;
    patch.function_count = 2U;

    for (index = 0; index < patch_count; ++index) {
        const int use_v2 = (index & 1) == 0;
        updates[0].target = erase(use_v2 ? first_v2 : first_v1);
        updates[1].target = erase(use_v2 ? second_v2 : second_v1);
        patch.expected_generation = generation;
        patch.candidate_generation = generation + 1U;
        CHECK(cvite_runtime_apply_patch(runtime, &patch, &error) == CVITE_STATUS_OK);
        generation += 1U;
        if ((index & 127) == 0) {
            CHECK(cvite_runtime_collect_retired(
                runtime, &reclaimed, &error) == CVITE_STATUS_OK);
            total_reclaimed += reclaimed;
        }
    }

    atomic_store_explicit(&context.stop, true, memory_order_relaxed);
    for (index = 0; index < reader_count; ++index) {
        CHECK(pthread_join(readers[index], NULL) == 0);
    }

    CHECK(atomic_load_explicit(&context.failures, memory_order_relaxed) == 0);
    CHECK(cvite_runtime_generation(runtime) == generation);
    CHECK(cvite_runtime_collect_retired(
        runtime, &reclaimed, &error) == CVITE_STATUS_OK);
    total_reclaimed += reclaimed;
    CHECK(total_reclaimed >= (size_t)patch_count);

    cvite_runtime_destroy(runtime);
    return 0;
}
