#include "run.h"
#include "run_internal.h"

#include "cvite/baseline.h"

#include <dirent.h>
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

#ifndef CVITE_LLVM_LINK_PATH
#define CVITE_LLVM_LINK_PATH "llvm-link-18"
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

    return path != NULL && stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool path_is_directory(const char *path)
{
    struct stat status;

    return path != NULL && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
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

static int copy_path(char output[PATH_MAX], const char *value)
{
    const size_t length = value != NULL ? strlen(value) : 0U;

    if (length == 0U || length >= PATH_MAX) {
        return -1;
    }
    (void)memcpy(output, value, length + 1U);
    return 0;
}

static int parent_directory(char path[PATH_MAX])
{
    char *separator;

    if (path == NULL || path[0] != '/') {
        return -1;
    }
    separator = strrchr(path, '/');
    if (separator == NULL) {
        return -1;
    }
    if (separator == path) {
        path[1] = '\0';
        return 0;
    }
    *separator = '\0';
    return 0;
}

static int resolve_project_input(
    const char *input,
    cvite_run_state *state,
    FILE *diagnostics)
{
    char resolved_input[PATH_MAX];
    char candidate_root[PATH_MAX];
    char candidate_source[PATH_MAX];
    const char *selected = input;
    bool has_root;
    bool has_source;

    if (input == NULL || state == NULL || diagnostics == NULL) {
        return -1;
    }

    if (path_is_directory(input)) {
        if (realpath(input, resolved_input) == NULL ||
            copy_path(state->project_root, resolved_input) != 0 ||
            cvite_join_path(
                candidate_root,
                sizeof(candidate_root),
                resolved_input,
                CVITE_SOURCE_ROOT_CANDIDATE) != 0 ||
            cvite_join_path(
                candidate_source,
                sizeof(candidate_source),
                resolved_input,
                CVITE_SOURCE_DIRECTORY_CANDIDATE) != 0) {
            (void)fprintf(diagnostics, "cvite: project path is unavailable\n");
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
                    "cvite: project discovery expects main.c or src/main.c\n");
            }
            return -1;
        }
        selected = has_root ? candidate_root : candidate_source;
    } else if (!path_is_regular_file(input)) {
        (void)fprintf(diagnostics, "cvite: source path does not exist: %s\n", input);
        return -1;
    }

    if (!string_ends_with(selected, ".c")) {
        (void)fprintf(diagnostics, "cvite: the entry source must be a .c file\n");
        return -1;
    }
    if (realpath(selected, state->source_path) == NULL) {
        (void)fprintf(
            diagnostics,
            "cvite: could not resolve source path '%s': %s\n",
            selected,
            strerror(errno));
        return -1;
    }

    if (!path_is_directory(input)) {
        if (copy_path(state->project_root, state->source_path) != 0 ||
            parent_directory(state->project_root) != 0) {
            (void)fprintf(diagnostics, "cvite: source path has no project root\n");
            return -1;
        }
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
    DIR *directory;
    struct dirent *entry;
    char path[PATH_MAX];

    if (state == NULL || state->build_directory[0] == '\0') {
        return;
    }
    directory = opendir(state->build_directory);
    if (directory == NULL) {
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (cvite_join_path(
                path,
                sizeof(path),
                state->build_directory,
                entry->d_name) == 0) {
            cvite_remove_if_present(path);
        }
    }
    (void)closedir(directory);
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
    if (copy_path(state->build_directory, directory) != 0) {
        (void)rmdir(directory);
        (void)fprintf(stderr, "cvite: temporary build path is too long\n");
        return -1;
    }
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

    if (cvite_compile_program(
            state,
            CVITE_BUILD_BASELINE,
            object_path) != CVITE_BUILD_RESULT_OK) {
        return -1;
    }

    status = cvite_orc_loader_create(&state->loader, &error);
    if (status != CVITE_STATUS_OK) {
        cvite_remove_if_present(object_path);
        cvite_print_runtime_error("ORC loader initialization", &error);
        return -1;
    }

    if (cvite_native_link_prepare(
            state->loader, state->source_path, state->clang_path) != 0) {
        cvite_remove_if_present(object_path);
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
    const char *link_path = environment_or_default(
        "CVITE_LLVM_LINK", CVITE_LLVM_LINK_PATH);
    const char *plugin_path = select_plugin_path();

    if (stream == NULL) {
        return;
    }
    (void)fprintf(stream, "compiler service: available\n");
    (void)fprintf(stream, "clang: %s\n", clang_path);
    (void)fprintf(stream, "opt: %s\n", opt_path);
    (void)fprintf(stream, "llvm-link: %s\n", link_path);
    (void)fprintf(stream, "pass plugin: %s\n", plugin_path);
    cvite_compile_database_print_doctor(stream);
    (void)fprintf(
        stream,
        "project mode: atomic multi-translation-unit refresh with TU IR cache\n");
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

    if (resolve_project_input(argv[0], &state, stderr) != 0) {
        return 2;
    }

    state.clang_path = environment_or_default("CVITE_CLANG", CVITE_CLANG_PATH);
    state.opt_path = environment_or_default("CVITE_OPT", CVITE_OPT_PATH);
    state.llvm_link_path = environment_or_default(
        "CVITE_LLVM_LINK", CVITE_LLVM_LINK_PATH);
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
    if (cvite_refresh_project(&state) != CVITE_PROJECT_RESULT_OK) {
        goto cleanup;
    }

    (void)fprintf(stderr, "[cvite] entry: %s\n", state.source_path);
    (void)fprintf(
        stderr,
        "[cvite] compiling baseline (%zu translation unit%s)...\n",
        state.translation_unit_count,
        state.translation_unit_count == 1U ? "" : "s");
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
        "[cvite] application started; watching linked project dependencies\n");
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
    cvite_discard_candidate_build(&state);
    cvite_free_project(&state);
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
