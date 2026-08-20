#include "run_internal.h"

#include "cvite/host.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#define CVITE_WATCH_DEBOUNCE_NS UINT64_C(50000000)
#define CVITE_WATCH_POLL_MS 100
#define CVITE_EVENT_BUFFER_SIZE (16U * 1024U)

static double elapsed_milliseconds(struct timespec start, struct timespec finish)
{
    const time_t seconds = finish.tv_sec - start.tv_sec;
    const long nanoseconds = finish.tv_nsec - start.tv_nsec;
    return (double)seconds * 1000.0 + (double)nanoseconds / 1000000.0;
}

static int publish_candidate(cvite_run_state *state)
{
    char object_path[PATH_MAX];
    cvite_orc_generation generation = CVITE_ORC_GENERATION_INVALID;
    cvite_patch patch = {0};
    cvite_error error = {0};
    cvite_status status;
    uint64_t active_generation;
    struct timespec started = {0, 0};
    struct timespec finished = {0, 0};

    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    if (cvite_compile_translation_unit(
            state,
            CVITE_BUILD_CANDIDATE,
            object_path) != 0) {
        return -1;
    }

    status = cvite_orc_loader_stage_object(
        state->loader,
        object_path,
        &generation,
        &error);
    cvite_remove_if_present(object_path);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("candidate linking", &error);
        return -1;
    }

    active_generation = cvite_host_generation();
    if (active_generation == UINT64_MAX) {
        (void)fprintf(stderr, "[cvite] runtime generation is exhausted\n");
        (void)cvite_orc_loader_discard_generation(
            state->loader,
            generation,
            &error);
        return -1;
    }

    status = cvite_orc_loader_prepare_patch(
        state->loader,
        generation,
        active_generation,
        active_generation + UINT64_C(1),
        &patch,
        &error);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("candidate validation", &error);
        (void)fprintf(stderr, "[cvite] previous code remains active\n");
        (void)cvite_orc_loader_discard_generation(
            state->loader,
            generation,
            &error);
        return -1;
    }

    status = cvite_host_apply_patch(&patch, &error);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("patch publication", &error);
        (void)fprintf(stderr, "[cvite] previous code remains active\n");
        (void)cvite_orc_loader_discard_generation(
            state->loader,
            generation,
            &error);
        return -1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    (void)fprintf(
        stderr,
        "[cvite] refreshed %zu function%s → generation %" PRIu64 " (%.1f ms)\n",
        patch.function_count,
        patch.function_count == 1U ? "" : "s",
        patch.candidate_generation,
        elapsed_milliseconds(started, finished));
    return 0;
}

static bool watch_event_matches(
    const struct inotify_event *event,
    const char *file_name)
{
    const uint32_t interesting = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
        IN_ATTRIB;

    if ((event->mask & IN_Q_OVERFLOW) != 0U) {
        return true;
    }
    if ((event->mask & interesting) == 0U || event->len == 0U) {
        return false;
    }
    return strcmp(event->name, file_name) == 0;
}

static void sleep_nanoseconds(uint64_t nanoseconds)
{
    struct timespec duration;

    duration.tv_sec = (time_t)(nanoseconds / UINT64_C(1000000000));
    duration.tv_nsec = (long)(nanoseconds % UINT64_C(1000000000));
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

static void drain_watch_events(int descriptor)
{
    char buffer[CVITE_EVENT_BUFFER_SIZE];
    ssize_t count;

    do {
        count = read(descriptor, buffer, sizeof(buffer));
    } while (count > 0 || (count < 0 && errno == EINTR));
}

int cvite_initialize_source_watcher(cvite_run_state *state)
{
    state->watch_descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (state->watch_descriptor < 0) {
        (void)fprintf(
            stderr,
            "[cvite] inotify initialization failed: %s\n",
            strerror(errno));
        return -1;
    }

    state->watch_handle = inotify_add_watch(
        state->watch_descriptor,
        state->watch_directory,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_ATTRIB |
            IN_DELETE_SELF | IN_MOVE_SELF);
    if (state->watch_handle < 0) {
        (void)fprintf(
            stderr,
            "[cvite] could not watch %s: %s\n",
            state->watch_directory,
            strerror(errno));
        (void)close(state->watch_descriptor);
        state->watch_descriptor = -1;
        return -1;
    }
    return 0;
}

void cvite_close_source_watcher(cvite_run_state *state)
{
    if (state->watch_descriptor < 0) {
        return;
    }
    if (state->watch_handle >= 0) {
        (void)inotify_rm_watch(state->watch_descriptor, state->watch_handle);
    }
    (void)close(state->watch_descriptor);
    state->watch_descriptor = -1;
    state->watch_handle = -1;
}

void *cvite_watch_source(void *opaque)
{
    cvite_run_state *state = opaque;
    struct pollfd poll_descriptor;
    char buffer[CVITE_EVENT_BUFFER_SIZE];

    poll_descriptor.fd = state->watch_descriptor;
    poll_descriptor.events = POLLIN;
    poll_descriptor.revents = 0;

    while (!atomic_load_explicit(
        &state->stop_requested,
        memory_order_acquire)) {
        int poll_status = poll(&poll_descriptor, 1U, CVITE_WATCH_POLL_MS);
        bool rebuild = false;

        if (poll_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)fprintf(
                stderr,
                "[cvite] source watcher failed: %s\n",
                strerror(errno));
            break;
        }
        if (poll_status == 0) {
            continue;
        }
        if ((poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            (void)fprintf(stderr, "[cvite] source watcher became unavailable\n");
            break;
        }
        if ((poll_descriptor.revents & POLLIN) == 0) {
            continue;
        }

        for (;;) {
            ssize_t count = read(state->watch_descriptor, buffer, sizeof(buffer));
            size_t offset = 0U;

            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                (void)fprintf(
                    stderr,
                    "[cvite] source watcher read failed: %s\n",
                    strerror(errno));
                rebuild = false;
                goto watcher_done;
            }
            if (count == 0) {
                break;
            }

            while (offset < (size_t)count) {
                const size_t remaining = (size_t)count - offset;
                const struct inotify_event *event;
                size_t event_size;

                if (remaining < sizeof(struct inotify_event)) {
                    (void)fprintf(stderr, "[cvite] malformed inotify event\n");
                    rebuild = false;
                    goto watcher_done;
                }
                event = (const struct inotify_event *)(const void *)(buffer + offset);
                event_size = sizeof(*event) + (size_t)event->len;
                if (event_size > remaining) {
                    (void)fprintf(stderr, "[cvite] malformed inotify event\n");
                    rebuild = false;
                    goto watcher_done;
                }
                if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0U) {
                    (void)fprintf(
                        stderr,
                        "[cvite] watched project directory disappeared\n");
                    rebuild = false;
                    goto watcher_done;
                }
                if (watch_event_matches(event, state->watch_name)) {
                    rebuild = true;
                }
                offset += event_size;
            }
        }

        if (rebuild) {
            sleep_nanoseconds(CVITE_WATCH_DEBOUNCE_NS);
            drain_watch_events(state->watch_descriptor);
            (void)publish_candidate(state);
        }
    }

watcher_done:
    cvite_close_source_watcher(state);
    return NULL;
}
