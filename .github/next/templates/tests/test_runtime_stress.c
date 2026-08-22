#define _POSIX_C_SOURCE 200809L

#include "cvite/runtime.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CVITE_STRESS_READER_COUNT 6U
#define CVITE_STRESS_PATCH_COUNT 5000U
#define CVITE_STRESS_SLOT_COUNT 4U

static atomic_int stop_readers;
static atomic_int failed;
static volatile sig_atomic_t signals_seen;

static void a0(void) {}
static void b0(void) {}
static void c0(void) {}
static void d0(void) {}
static void a1(void) {}
static void b1(void) {}
static void c1(void) {}
static void d1(void) {}

static const cvite_function_pointer version_zero[CVITE_STRESS_SLOT_COUNT] = {
    a0, b0, c0, d0,
};
static const cvite_function_pointer version_one[CVITE_STRESS_SLOT_COUNT] = {
    a1, b1, c1, d1,
};

static void on_signal(int number)
{
    (void)number;
    ++signals_seen;
}

static int matches_version(
    const cvite_dispatch_view *view,
    const size_t *slots,
    const cvite_function_pointer *version)
{
    size_t index = 0U;
    for (index = 0U; index < CVITE_STRESS_SLOT_COUNT; ++index) {
        if (cvite_dispatch_view_target(view, slots[index]) != version[index]) {
            return 0;
        }
    }
    return 1;
}

typedef struct reader_context {
    const cvite_runtime *runtime;
    size_t slots[CVITE_STRESS_SLOT_COUNT];
} reader_context;

static void *reader_main(void *argument)
{
    const reader_context *context = (const reader_context *)argument;
    while (atomic_load_explicit(&stop_readers, memory_order_acquire) == 0) {
        cvite_dispatch_view view;
        cvite_error error;
        cvite_error_clear(&error);
        if (cvite_runtime_acquire_view(context->runtime, &view, &error) !=
            CVITE_STATUS_OK) {
            atomic_store_explicit(&failed, 1, memory_order_release);
            break;
        }
        if (!matches_version(&view, context->slots, version_zero) &&
            !matches_version(&view, context->slots, version_one)) {
            atomic_store_explicit(&failed, 1, memory_order_release);
            break;
        }
        cvite_function_pointer target =
            cvite_dispatch_view_target(&view, context->slots[0]);
        if (target == NULL) {
            atomic_store_explicit(&failed, 1, memory_order_release);
            break;
        }
        target();
    }
    return NULL;
}

static void *signal_main(void *argument)
{
    (void)argument;
    while (atomic_load_explicit(&stop_readers, memory_order_acquire) == 0) {
        (void)kill(getpid(), SIGUSR1);
        sched_yield();
    }
    return NULL;
}

static int register_functions(
    cvite_runtime *runtime,
    size_t *slots,
    cvite_error *error)
{
    static const char *names[CVITE_STRESS_SLOT_COUNT] = {
        "stress_a", "stress_b", "stress_c", "stress_d",
    };
    size_t index = 0U;
    for (index = 0U; index < CVITE_STRESS_SLOT_COUNT; ++index) {
        const cvite_function_definition definition = {
            {UINT64_C(0x1000), UINT64_C(0x2000) + (uint64_t)index},
            {UINT64_C(0x3000), UINT64_C(0x4000) + (uint64_t)index},
            version_zero[index],
            names[index],
        };
        if (cvite_runtime_register_function(
                runtime, &definition, &slots[index], error) != CVITE_STATUS_OK) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    cvite_runtime *runtime = NULL;
    cvite_error error;
    cvite_error_clear(&error);
    if (cvite_runtime_create(&runtime, &error) != CVITE_STATUS_OK) {
        fprintf(stderr, "runtime create failed: %s\n", error.message);
        return 1;
    }

    reader_context context;
    context.runtime = runtime;
    if (!register_functions(runtime, context.slots, &error) ||
        cvite_runtime_seal(runtime, &error) != CVITE_STATUS_OK) {
        fprintf(stderr, "runtime setup failed: %s\n", error.message);
        cvite_runtime_destroy(runtime);
        return 1;
    }

    atomic_init(&stop_readers, 0);
    atomic_init(&failed, 0);
    signals_seen = 0;

    pthread_t readers[CVITE_STRESS_READER_COUNT];
    pthread_t signal_thread;
    size_t created_readers = 0U;
    for (created_readers = 0U;
         created_readers < CVITE_STRESS_READER_COUNT;
         ++created_readers) {
        if (pthread_create(
                &readers[created_readers], NULL, reader_main, &context) != 0) {
            atomic_store_explicit(&failed, 1, memory_order_release);
            break;
        }
    }
    const int signal_thread_created =
        pthread_create(&signal_thread, NULL, signal_main, NULL) == 0;
    if (!signal_thread_created) {
        atomic_store_explicit(&failed, 1, memory_order_release);
    }

    size_t iteration = 0U;
    for (iteration = 0U;
         iteration < CVITE_STRESS_PATCH_COUNT &&
         atomic_load_explicit(&failed, memory_order_acquire) == 0;
         ++iteration) {
        cvite_function_update updates[CVITE_STRESS_SLOT_COUNT];
        const cvite_function_pointer *version =
            (iteration % 2U) == 0U ? version_one : version_zero;
        size_t index = 0U;
        for (index = 0U; index < CVITE_STRESS_SLOT_COUNT; ++index) {
            updates[index].id.high = UINT64_C(0x1000);
            updates[index].id.low = UINT64_C(0x2000) + (uint64_t)index;
            updates[index].abi_fingerprint.high = UINT64_C(0x3000);
            updates[index].abi_fingerprint.low =
                UINT64_C(0x4000) + (uint64_t)index;
            updates[index].target = version[index];
        }
        const uint64_t generation = cvite_runtime_generation(runtime);
        const cvite_patch patch = {
            generation,
            generation + UINT64_C(1),
            updates,
            CVITE_STRESS_SLOT_COUNT,
        };
        cvite_error_clear(&error);
        if (cvite_runtime_apply_patch(runtime, &patch, &error) !=
            CVITE_STATUS_OK) {
            fprintf(stderr, "patch failed: %s\n", error.message);
            atomic_store_explicit(&failed, 1, memory_order_release);
            break;
        }
    }

    atomic_store_explicit(&stop_readers, 1, memory_order_release);
    for (size_t index = 0U; index < created_readers; ++index) {
        (void)pthread_join(readers[index], NULL);
    }
    if (signal_thread_created) {
        (void)pthread_join(signal_thread, NULL);
    }

    const int result =
        atomic_load_explicit(&failed, memory_order_acquire) != 0 ||
        signals_seen == 0
        ? 1
        : 0;
    if (result != 0) {
        fprintf(stderr, "runtime stress failed; signals=%d\n", (int)signals_seen);
    } else {
        printf(
            "runtime stress passed: %u atomic generations, %d signals\n",
            (unsigned)CVITE_STRESS_PATCH_COUNT,
            (int)signals_seen);
    }
    cvite_runtime_destroy(runtime);
    return result;
}
