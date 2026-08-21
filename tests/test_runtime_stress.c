#include "cvite/runtime.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STRESS_SLOT_COUNT 4U
#define STRESS_PATCH_COUNT 12000U
#define STRESS_READER_COUNT 6U

static cvite_runtime *stress_runtime;
static size_t stress_slots[STRESS_SLOT_COUNT];
static atomic_bool stress_stop;
static atomic_uint stress_mixed_views;
static atomic_uint stress_runtime_errors;
static volatile sig_atomic_t stress_signal_count;

static void target_a0(void) {}
static void target_a1(void) {}
static void target_a2(void) {}
static void target_a3(void) {}
static void target_b0(void) {}
static void target_b1(void) {}
static void target_b2(void) {}
static void target_b3(void) {}

static const cvite_function_pointer targets_a[STRESS_SLOT_COUNT] = {
    target_a0,
    target_a1,
    target_a2,
    target_a3,
};

static const cvite_function_pointer targets_b[STRESS_SLOT_COUNT] = {
    target_b0,
    target_b1,
    target_b2,
    target_b3,
};

static const cvite_id function_ids[STRESS_SLOT_COUNT] = {
    CVITE_ID(UINT64_C(0xA100), UINT64_C(1)),
    CVITE_ID(UINT64_C(0xA100), UINT64_C(2)),
    CVITE_ID(UINT64_C(0xA100), UINT64_C(3)),
    CVITE_ID(UINT64_C(0xA100), UINT64_C(4)),
};

static const cvite_id function_abi =
    CVITE_ID(UINT64_C(0xB200), UINT64_C(1));

static void stress_signal_handler(int signal_number)
{
    (void)signal_number;
    ++stress_signal_count;
}

static bool view_matches(
    const cvite_dispatch_view *view,
    const cvite_function_pointer targets[STRESS_SLOT_COUNT])
{
    size_t index = 0U;
    for (index = 0U; index < STRESS_SLOT_COUNT; ++index) {
        if (cvite_dispatch_view_target(view, stress_slots[index]) !=
            targets[index]) {
            return false;
        }
    }
    return true;
}

static void *stress_reader(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&stress_stop, memory_order_acquire)) {
        cvite_dispatch_view view;
        cvite_error error;
        if (cvite_runtime_acquire_view(stress_runtime, &view, &error) !=
            CVITE_STATUS_OK) {
            atomic_fetch_add_explicit(
                &stress_runtime_errors, 1U, memory_order_relaxed);
            continue;
        }
        if (!view_matches(&view, targets_a) &&
            !view_matches(&view, targets_b)) {
            atomic_fetch_add_explicit(
                &stress_mixed_views, 1U, memory_order_relaxed);
        }
    }
    return NULL;
}

static int fail(const char *operation, const cvite_error *error)
{
    fprintf(
        stderr,
        "runtime stress: %s failed: %s\n",
        operation,
        error != NULL && error->message[0] != '\0'
            ? error->message
            : "unknown error");
    return 1;
}

int main(void)
{
    cvite_error error;
    pthread_t readers[STRESS_READER_COUNT];
    struct sigaction action;
    size_t index = 0U;
    uint64_t generation = 0U;

    memset(&action, 0, sizeof(action));
    action.sa_handler = stress_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) {
        perror("runtime stress: sigaction");
        return 1;
    }

    if (cvite_runtime_create(&stress_runtime, &error) != CVITE_STATUS_OK) {
        return fail("runtime creation", &error);
    }

    for (index = 0U; index < STRESS_SLOT_COUNT; ++index) {
        char name[32];
        cvite_function_definition definition;
        (void)snprintf(name, sizeof(name), "stress_slot_%zu", index);
        definition = (cvite_function_definition){
            function_ids[index],
            function_abi,
            targets_a[index],
            name,
        };
        if (cvite_runtime_register_function(
                stress_runtime,
                &definition,
                &stress_slots[index],
                &error) != CVITE_STATUS_OK) {
            cvite_runtime_destroy(stress_runtime);
            return fail("function registration", &error);
        }
    }

    if (cvite_runtime_seal(stress_runtime, &error) != CVITE_STATUS_OK) {
        cvite_runtime_destroy(stress_runtime);
        return fail("runtime sealing", &error);
    }

    atomic_init(&stress_stop, false);
    atomic_init(&stress_mixed_views, 0U);
    atomic_init(&stress_runtime_errors, 0U);
    for (index = 0U; index < STRESS_READER_COUNT; ++index) {
        if (pthread_create(&readers[index], NULL, stress_reader, NULL) != 0) {
            perror("runtime stress: pthread_create");
            atomic_store_explicit(&stress_stop, true, memory_order_release);
            while (index > 0U) {
                --index;
                (void)pthread_join(readers[index], NULL);
            }
            cvite_runtime_destroy(stress_runtime);
            return 1;
        }
    }

    generation = cvite_runtime_generation(stress_runtime);
    for (index = 0U; index < STRESS_PATCH_COUNT; ++index) {
        cvite_function_update updates[STRESS_SLOT_COUNT];
        const cvite_function_pointer *targets =
            (index & 1U) == 0U ? targets_b : targets_a;
        cvite_patch patch;
        size_t slot = 0U;
        for (slot = 0U; slot < STRESS_SLOT_COUNT; ++slot) {
            updates[slot] = (cvite_function_update){
                function_ids[slot],
                function_abi,
                targets[slot],
            };
        }
        patch = (cvite_patch){
            generation,
            generation + UINT64_C(1),
            updates,
            STRESS_SLOT_COUNT,
        };
        if (cvite_runtime_apply_patch(stress_runtime, &patch, &error) !=
            CVITE_STATUS_OK) {
            atomic_store_explicit(&stress_stop, true, memory_order_release);
            for (slot = 0U; slot < STRESS_READER_COUNT; ++slot) {
                (void)pthread_join(readers[slot], NULL);
            }
            cvite_runtime_destroy(stress_runtime);
            return fail("patch publication", &error);
        }
        ++generation;
        if ((index % 17U) == 0U) {
            (void)kill(getpid(), SIGUSR1);
        }
    }

    atomic_store_explicit(&stress_stop, true, memory_order_release);
    for (index = 0U; index < STRESS_READER_COUNT; ++index) {
        (void)pthread_join(readers[index], NULL);
    }

    if (atomic_load_explicit(&stress_mixed_views, memory_order_relaxed) != 0U) {
        fprintf(stderr, "runtime stress: a reader observed a mixed generation\n");
        cvite_runtime_destroy(stress_runtime);
        return 1;
    }
    if (atomic_load_explicit(&stress_runtime_errors, memory_order_relaxed) != 0U) {
        fprintf(stderr, "runtime stress: a reader could not acquire a view\n");
        cvite_runtime_destroy(stress_runtime);
        return 1;
    }
    if (stress_signal_count == 0) {
        fprintf(stderr, "runtime stress: no asynchronous signal was delivered\n");
        cvite_runtime_destroy(stress_runtime);
        return 1;
    }
    if (cvite_runtime_generation(stress_runtime) != generation) {
        fprintf(stderr, "runtime stress: final generation mismatch\n");
        cvite_runtime_destroy(stress_runtime);
        return 1;
    }

    cvite_runtime_destroy(stress_runtime);
    return 0;
}
