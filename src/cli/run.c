#include "run.h"
#include "run_internal.h"

#include "cvite/baseline.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CVITE_CLANG_PATH
#define CVITE_CLANG_PATH "clang-18"
#endif

#ifndef CVITE_OPT_PATH
#define CVITE_OPT_PATH "opt-18"
#endif

#ifndef CVITE_PASS_PLUGIN_BUILD_PATH
#define CVITE_PASS_PLUGIN_BUILD_PATH "CViteLoweringPass.so"
#endif

#ifndef CVITE_PASS_PLUGIN_INSTALL_PATH
#define CVITE_PASS_PLUGIN_INSTALL_PATH "CViteLoweringPass.so"
#endif

#define CVITE_SOURCE_DIRECTORY_CANDIDATE "src/main.c"
#define CVITE_SOURCE_ROOT_CANDIDATE "main.c"
#define CVITE_BUILD_TEMPLATE "/tmp/cvite-run-XXXXXX"

static bool path_is_regular_file(const char *path)
{
    struct stat status;

    if (path == NULL || stat(path, &status) != 0) {
        return false;
    }
    return S_ISREG(status.st_mode);
}

static bool path_is_directory(const char *path)
{
    struct stat status;

    if (path == NULL || stat(path, &status) != 0) {
        return false;
    }
    return S_ISDIR(status.st_mode);
}

static bool string_ends_with(const char *value, const char *suffix)
{
    size_t value_length;
    size_t suffix_length;

    if (value == NULL || suffix == NULL) {
        return false;
    }
    value_length = strlen(value);
    suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
        strcmp(value + value_length - suffix_length, suffix) == 0;
}

static int resolve_source_path(
    const char *input,
    char output[PATH_MAX],
    FILE *diagnostics)
{
    char candidate_root[PATH_MAX];
    char candidate_source[PATH_MAX];
    const char *selected = input;
    bool has_root;
    bool has_source;

    if (input == NULL || output == NULL || diagnostics == NULL) {
        return -1;
    }

    if (path_is_directory(input)) {
        if (cvite_join_path(
                candidate_root,
                sizeof(candidate_root),
                input,
                CVITE_SOURCE_ROOT_CANDIDATE) != 0 ||
            cvite_join_path(
                candidate_source,
                sizeof(candidate_source),
                input,
                CVITE_SOURCE_DIRECTORY_CANDIDATE) != 0) {
            (void)fprintf(diagnostics, "cvite: project path is too long\n");
            return -1;
        }

        has_root = path_is_regular_file(candidate_root);
        has_source = path_is_regular_file(candidate_source);
        if (has_root == has_source) {
            if (has_root) {
                (void)fprintf(
                    diagnostics,
                    "cvite: project discovery is ambiguous; both main.c and "
                    "src/main.c exist\n");
            } else {
                (void)fprintf(
                    diagnostics,
                    "cvite: M1 project discovery expects main.c or src/main.c\n");
            }
            return -1;
        }
        selected = has_root ? candidate_root : candidate_source;
    } else if (!path_is_regular_file(input)) {
        (void)fprintf(diagnostics, "cvite: source path does not exist: %s\n", input);
        return -1;
    }

    if (!string_ends_with(selected, ".c")) {
        (void)fprintf(
            diagnostics,
            "cvite: the M1 runner currently accepts one .c translation unit\n");
        return -1;
    }

    if (realpath(selected, output) == NULL) {
        (void)fprintf(
            diagnostics,
            "cvite: could not resolve source path '%s': %s\n",
            selected,
            strerror(errno));
        return -1;
    }
    return 0;
}

static int split_watch_path(cvite_run_state *state, FILE *diagnostics)
{
    char *separator;
    const size_t source_length = strlen(state->source_path);

    if (source_length == 0U || source_length >= sizeof(state->watch_directory)) {
        return -1;
    }
    (void)memcpy(
        state->watch_directory,
        state->source_path,
        source_length + 1U);
    separator = strrchr(state->watch_directory, '/');
    if (separator == NULL || separator[1] == '\0') {
        (void)fprintf(diagnostics, "cvite: source path has no file name\n");
        return -1;
    }
    if (strlen(separator + 1) >= sizeof(state->watch_name)) {
        (void)fprintf(diagnostics, "cvite: source file name is too long\n");
        return -1;
    }
    (void)strcpy(state->watch_name, separator + 1);
    if (separator == state->watch_directory) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    return 0;
}

static const char *environment_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static const char *select_plugin_path(void)
{
    const char *override = getenv("CVITE_PASS_PLUGIN");

    if (override != NULL && override[0] != '\0') {
        return override;
    }
    if (access(CVITE_PASS_PLUGIN_BUILD_PATH, R_OK) == 0) {
        return CVITE_PASS_PLUGIN_BUILD_PATH;
    }
    return CVITE_PASS_PLUGIN_INSTALL_PATH;
}

static void cleanup_build_directory(const cvite_run_state *state)
{
    char path[PATH_MAX];

    if (cvite_build_path(state, "translation.raw.ll", path) == 0) {
        cvite_remove_if_present(path);
    }
    if (cvite_build_path(state, "translation.ll", path) == 0) {
        cvite_remove_if_present(path);
    }
    if (cvite_build_path(state, "translation.o", path) == 0) {
        cvite_remove_if_present(path);
    }
    if (cvite_dependency_path(state, CVITE_BUILD_BASELINE, path) == 0) {
        cvite_remove_if_present(path);
    }
    if (cvite_dependency_path(state, CVITE_BUILD_CANDIDATE, path) == 0) {
        cvite_remove_if_present(path);
    }
    (void)rmdir(state->build_directory);
}

static int initialize_build_directory(cvite_run_state *state)
{
    char template_path[] = CVITE_BUILD_TEMPLATE;
    char *directory = mkdtemp(template_path);

    if (directory == NULL) {
        (void)fprintf(
            stderr,
            "cvite: could not create temporary build directory: %s\n",
            strerror(errno));
        return -1;
    }
    if (strlen(directory) >= sizeof(state->build_directory)) {
        (void)rmdir(directory);
        (void)fprintf(stderr, "cvite: temporary build path is too long\n");
        return -1;
    }
    (void)strcpy(state->build_directory, directory);
    return 0;
}

static int prepare_baseline(
    cvite_run_state *state,
    cvite_program_main *program_main)
{
    char object_path[PATH_MAX];
    cvite_orc_generation generation = CVITE_ORC_GENERATION_INVALID;
    cvite_error error = {0};
    cvite_status status;

    if (cvite_compile_translation_unit(
            state,
            CVITE_BUILD_BASELINE,
            object_path) != 0) {
        return -1;
    }

    status = cvite_orc_loader_create(&state->loader, &error);
    if (status != CVITE_STATUS_OK) {
        cvite_remove_if_present(object_path);
        cvite_print_runtime_error("ORC loader initialization", &error);
        return -1;
    }

    status = cvite_orc_loader_stage_object(
        state->loader,
        object_path,
        &generation,
        &error);
    cvite_remove_if_present(object_path);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("baseline linking", &error);
        return -1;
    }

    status = cvite_orc_loader_prepare_baseline(
        state->loader,
        generation,
        program_main,
        &error);
    if (status != CVITE_STATUS_OK) {
        cvite_print_runtime_error("baseline preparation", &error);
        return -1;
    }
    return 0;
}

static char **make_program_arguments(
    const char *source_path,
    int argument_count,
    char **arguments)
{
    const size_t count = (size_t)argument_count + 2U;
    char **program_arguments = calloc(count, sizeof(*program_arguments));
    int index;

    if (program_arguments == NULL) {
        return NULL;
    }
    program_arguments[0] = (char *)source_path;
    for (index = 0; index < argument_count; ++index) {
        program_arguments[index + 1] = arguments[index];
    }
    return program_arguments;
}

void cvite_run_print_doctor(FILE *stream)
{
    const char *clang_path = environment_or_default("CVITE_CLANG", CVITE_CLANG_PATH);
    const char *opt_path = environment_or_default("CVITE_OPT", CVITE_OPT_PATH);
    const char *plugin_path = select_plugin_path();

    if (stream == NULL) {
        return;
    }
    (void)fprintf(stream, "compiler service: available\n");
    (void)fprintf(stream, "clang: %s\n", clang_path);
    (void)fprintf(stream, "opt: %s\n", opt_path);
    (void)fprintf(stream, "pass plugin: %s\n", plugin_path);
    (void)fprintf(
        stream,
        "project mode: single translation unit + header graph (M1)\n");
}

int cvite_run_command(int argc, char **argv)
{
    cvite_run_state state;
    cvite_program_main program_main = NULL;
    pthread_t watcher;
    bool watcher_started = false;
    bool application_started = false;
    char **program_arguments = NULL;
    int application_argument_count = 0;
    char **application_arguments = NULL;
    int result = 1;
    int thread_status;

    (void)memset(&state, 0, sizeof(state));
    state.watch_descriptor = -1;
    atomic_init(&state.stop_requested, false);

    if (argc < 1) {
        (void)fprintf(
            stderr,
            "Usage: cvite run <source.c|project-directory> [-- <program-args...>]\n");
        return 2;
    }
    if (argc > 1) {
        if (strcmp(argv[1], "--") != 0) {
            (void)fprintf(
                stderr,
                "cvite: program arguments must follow --\n");
            return 2;
        }
        application_argument_count = argc - 2;
        application_arguments = argv + 2;
    }

    if (resolve_source_path(argv[0], state.source_path, stderr) != 0 ||
        split_watch_path(&state, stderr) != 0) {
        return 2;
    }

    state.clang_path = environment_or_default("CVITE_CLANG", CVITE_CLANG_PATH);
    state.opt_path = environment_or_default("CVITE_OPT", CVITE_OPT_PATH);
    state.plugin_path = select_plugin_path();

    if (access(state.plugin_path, R_OK) != 0) {
        (void)fprintf(
            stderr,
            "cvite: LLVM pass plugin is not readable: %s\n"
            "       set CVITE_PASS_PLUGIN to override it\n",
            state.plugin_path);
        return 3;
    }
    if (initialize_build_directory(&state) != 0) {
        return 3;
    }

    (void)fprintf(stderr, "[cvite] source: %s\n", state.source_path);
    (void)fprintf(stderr, "[cvite] compiling baseline...\n");
    if (prepare_baseline(&state, &program_main) != 0) {
        goto cleanup;
    }

    program_arguments = make_program_arguments(
        state.source_path,
        application_argument_count,
        application_arguments);
    if (program_arguments == NULL) {
        (void)fprintf(stderr, "cvite: could not allocate program arguments\n");
        goto cleanup;
    }

    if (cvite_initialize_source_watcher(&state) != 0) {
        goto cleanup;
    }

    thread_status = pthread_create(&watcher, NULL, cvite_watch_source, &state);
    if (thread_status != 0) {
        (void)fprintf(
            stderr,
            "cvite: could not start source watcher: %s\n",
            strerror(thread_status));
        cvite_close_source_watcher(&state);
        goto cleanup;
    }
    watcher_started = true;

    (void)fprintf(
        stderr,
        "[cvite] application started; watching Clang dependency graph\n");
    application_started = true;
    result = program_main(
        application_argument_count + 1,
        program_arguments);
    (void)fprintf(stderr, "[cvite] application exited with status %d\n", result);

cleanup:
    atomic_store_explicit(
        &state.stop_requested,
        true,
        memory_order_release);
    if (watcher_started) {
        (void)pthread_join(watcher, NULL);
    }
    free(program_arguments);
    /*
     * Once user code has run, retain JIT generations through process exit.
     * atexit handlers or surviving application threads may still reference
     * them, and quiescence-based reclamation is not implemented yet.
     */
    if (state.loader != NULL && !application_started) {
        cvite_orc_loader_destroy(state.loader);
    }
    cleanup_build_directory(&state);
    return result;
}
