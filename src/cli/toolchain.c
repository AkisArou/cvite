#include "run_internal.h"

#include "cvite/status.h"

#include <errno.h>
#include <stdbool.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define CVITE_UNIT_NAME_CAPACITY 96U

static bool verbose_commands(void)
{
    const char *value = getenv("CVITE_VERBOSE");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void print_command(char *const arguments[])
{
    size_t index = 0U;

    (void)fprintf(stderr, "[cvite] $");
    while (arguments[index] != NULL) {
        (void)fprintf(stderr, " %s", arguments[index]);
        ++index;
    }
    (void)fputc('\n', stderr);
}

static int run_process(char *const arguments[])
{
    pid_t process = (pid_t)0;
    int spawn_status;
    int wait_status = 0;
    pid_t waited;

    if (verbose_commands()) {
        print_command(arguments);
    }

    spawn_status = posix_spawnp(
        &process,
        arguments[0],
        NULL,
        NULL,
        arguments,
        environ);
    if (spawn_status != 0) {
        (void)fprintf(
            stderr,
            "[cvite] could not start %s: %s\n",
            arguments[0],
            strerror(spawn_status));
        return -1;
    }

    do {
        waited = waitpid(process, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        (void)fprintf(stderr, "[cvite] waitpid failed: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        (void)fprintf(
            stderr,
            "[cvite] %s terminated by signal %d\n",
            arguments[0],
            WTERMSIG(wait_status));
    }
    return -1;
}

int cvite_join_path(
    char *output,
    size_t output_size,
    const char *left,
    const char *right)
{
    int length;
    const bool has_separator = left != NULL && left[0] != '\0' &&
        left[strlen(left) - 1U] == '/';

    if (output == NULL || output_size == 0U || left == NULL || right == NULL) {
        return -1;
    }
    length = snprintf(
        output,
        output_size,
        has_separator ? "%s%s" : "%s/%s",
        left,
        right);
    if (length < 0 || (size_t)length >= output_size) {
        return -1;
    }
    return 0;
}

int cvite_build_path(
    const cvite_run_state *state,
    const char *name,
    char output[PATH_MAX])
{
    if (state == NULL) {
        return -1;
    }
    return cvite_join_path(output, PATH_MAX, state->build_directory, name);
}

void cvite_remove_if_present(const char *path)
{
    if (path != NULL) {
        (void)unlink(path);
    }
}

static int unit_artifact_path(
    const cvite_run_state *state,
    size_t unit_index,
    const char *state_name,
    const char *extension,
    char output[PATH_MAX])
{
    char name[CVITE_UNIT_NAME_CAPACITY];
    int length;

    length = snprintf(
        name,
        sizeof(name),
        "tu-%04zu.%s.%s",
        unit_index,
        state_name,
        extension);
    if (length < 0 || (size_t)length >= sizeof(name)) {
        return -1;
    }
    return cvite_build_path(state, name, output);
}

int cvite_active_ir_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX])
{
    return unit_artifact_path(state, unit_index, "active", "ll", output);
}

int cvite_active_dependency_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX])
{
    return unit_artifact_path(state, unit_index, "active", "d", output);
}

static int staged_ir_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX])
{
    return unit_artifact_path(state, unit_index, "staged", "ll", output);
}

static int staged_dependency_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX])
{
    return unit_artifact_path(state, unit_index, "staged", "d", output);
}

static int raw_ir_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX])
{
    return unit_artifact_path(state, unit_index, "raw", "ll", output);
}

static void report_build_failure(
    cvite_build_kind kind,
    const char *phase,
    const char *source_path)
{
    (void)fprintf(
        stderr,
        "[cvite] %s %s failed for %s%s\n",
        kind == CVITE_BUILD_BASELINE ? "baseline" : "candidate",
        phase,
        source_path != NULL ? source_path : "linked program",
        kind == CVITE_BUILD_BASELINE
            ? ""
            : "; previous code remains active");
}

static int compile_translation_unit(
    const cvite_run_state *state,
    size_t unit_index,
    cvite_build_kind kind,
    const char *output_ir,
    const char *dependency_path)
{
    const cvite_translation_unit *unit = &state->translation_units[unit_index];
    char raw_path[PATH_MAX];
    char plugin_argument[PATH_MAX + 32U];
    char target_name[64];
    char **clang_arguments = NULL;
    char *origin_arguments[9];
    size_t argument_capacity;
    size_t argument_index = 0U;
    size_t index;
    int length;
    int status;
    int result = -1;

    if (raw_ir_path(state, unit_index, raw_path) != 0) {
        return -1;
    }
    length = snprintf(
        plugin_argument,
        sizeof(plugin_argument),
        "-load-pass-plugin=%s",
        state->plugin_path);
    if (length < 0 || (size_t)length >= sizeof(plugin_argument)) {
        return -1;
    }
    length = snprintf(
        target_name,
        sizeof(target_name),
        "cvite-tu-%zu",
        unit_index);
    if (length < 0 || (size_t)length >= sizeof(target_name)) {
        return -1;
    }

    argument_capacity = unit->compile_argument_count + 24U;
    if (argument_capacity > SIZE_MAX / sizeof(*clang_arguments)) {
        return -1;
    }
    clang_arguments = calloc(argument_capacity, sizeof(*clang_arguments));
    if (clang_arguments == NULL) {
        return -1;
    }

    clang_arguments[argument_index++] = (char *)state->clang_path;
    clang_arguments[argument_index++] = (char *)"-std=c11";
    for (index = 0U; index < unit->compile_argument_count; ++index) {
        clang_arguments[argument_index++] = unit->compile_arguments[index];
    }
    clang_arguments[argument_index++] = (char *)"-O0";
    clang_arguments[argument_index++] = (char *)"-g";
    clang_arguments[argument_index++] = (char *)"-fPIC";
    clang_arguments[argument_index++] = (char *)"-fno-inline";
    clang_arguments[argument_index++] = (char *)"-fno-omit-frame-pointer";
    clang_arguments[argument_index++] = (char *)"-fno-discard-value-names";
    clang_arguments[argument_index++] = (char *)"-Xclang";
    clang_arguments[argument_index++] = (char *)"-disable-O0-optnone";
    clang_arguments[argument_index++] = (char *)"-MMD";
    clang_arguments[argument_index++] = (char *)"-MF";
    clang_arguments[argument_index++] = (char *)dependency_path;
    clang_arguments[argument_index++] = (char *)"-MT";
    clang_arguments[argument_index++] = target_name;
    clang_arguments[argument_index++] = (char *)"-S";
    clang_arguments[argument_index++] = (char *)"-emit-llvm";
    clang_arguments[argument_index++] = (char *)unit->source_path;
    clang_arguments[argument_index++] = (char *)"-o";
    clang_arguments[argument_index++] = raw_path;
    clang_arguments[argument_index] = NULL;

    origin_arguments[0] = (char *)state->opt_path;
    origin_arguments[1] = plugin_argument;
    origin_arguments[2] = (char *)"-passes=cvite-origin,verify";
    origin_arguments[3] = (char *)"-S";
    origin_arguments[4] = raw_path;
    origin_arguments[5] = (char *)"-o";
    origin_arguments[6] = (char *)output_ir;
    origin_arguments[7] = NULL;
    origin_arguments[8] = NULL;

    cvite_remove_if_present(raw_path);
    cvite_remove_if_present(output_ir);
    cvite_remove_if_present(dependency_path);

    status = run_process(clang_arguments);
    if (status != 0) {
        report_build_failure(kind, "compilation", unit->source_path);
        goto cleanup;
    }
    status = run_process(origin_arguments);
    if (status != 0) {
        report_build_failure(kind, "origin tagging", unit->source_path);
        goto cleanup;
    }
    result = 0;

cleanup:
    cvite_remove_if_present(raw_path);
    if (result != 0) {
        cvite_remove_if_present(output_ir);
        cvite_remove_if_present(dependency_path);
    }
    free(clang_arguments);
    return result;
}

static int compare_timespec(struct timespec left, struct timespec right)
{
    if (left.tv_sec != right.tv_sec) {
        return left.tv_sec < right.tv_sec ? -1 : 1;
    }
    if (left.tv_nsec != right.tv_nsec) {
        return left.tv_nsec < right.tv_nsec ? -1 : 1;
    }
    return 0;
}

static struct timespec newest_file_time(const struct stat *status)
{
    return compare_timespec(status->st_mtim, status->st_ctim) >= 0
        ? status->st_mtim
        : status->st_ctim;
}

typedef struct cvite_dirty_context {
    struct timespec compiled_at;
    bool dirty;
} cvite_dirty_context;

static int dependency_is_newer(const char *path, void *opaque)
{
    cvite_dirty_context *context = opaque;
    struct stat status;

    if (stat(path, &status) != 0 ||
        compare_timespec(newest_file_time(&status), context->compiled_at) > 0) {
        context->dirty = true;
    }
    return 0;
}

static bool translation_unit_is_dirty(
    const cvite_run_state *state,
    size_t unit_index)
{
    char ir_path[PATH_MAX];
    char dependency_path[PATH_MAX];
    struct stat ir_status;
    cvite_dirty_context context;

    if (cvite_active_ir_path(state, unit_index, ir_path) != 0 ||
        cvite_active_dependency_path(state, unit_index, dependency_path) != 0 ||
        stat(ir_path, &ir_status) != 0) {
        return true;
    }
    context.compiled_at = newest_file_time(&ir_status);
    context.dirty = false;
    if (cvite_visit_dependency_file(
            dependency_path,
            dependency_is_newer,
            &context) != 0) {
        return true;
    }
    return context.dirty;
}

static int allocate_dirty_set(cvite_run_state *state)
{
    if (state->translation_unit_count == 0U) {
        return -1;
    }
    if (state->translation_unit_count > SIZE_MAX / sizeof(*state->candidate_dirty)) {
        return -1;
    }
    state->candidate_dirty = calloc(
        state->translation_unit_count,
        sizeof(*state->candidate_dirty));
    if (state->candidate_dirty == NULL) {
        return -1;
    }
    state->candidate_dirty_count = state->translation_unit_count;
    return 0;
}

static int compile_selected_units(
    cvite_run_state *state,
    cvite_build_kind kind)
{
    size_t index;
    size_t compiled_count = 0U;

    if (kind == CVITE_BUILD_CANDIDATE && allocate_dirty_set(state) != 0) {
        return -1;
    }

    for (index = 0U; index < state->translation_unit_count; ++index) {
        char ir_path[PATH_MAX];
        char dependency_path[PATH_MAX];
        bool compile_unit = kind == CVITE_BUILD_BASELINE ||
            translation_unit_is_dirty(state, index);

        if (kind == CVITE_BUILD_CANDIDATE) {
            state->candidate_dirty[index] = compile_unit ? 1U : 0U;
        }
        if (!compile_unit) {
            continue;
        }
        if ((kind == CVITE_BUILD_BASELINE
                 ? cvite_active_ir_path(state, index, ir_path)
                 : staged_ir_path(state, index, ir_path)) != 0 ||
            (kind == CVITE_BUILD_BASELINE
                 ? cvite_active_dependency_path(state, index, dependency_path)
                 : staged_dependency_path(state, index, dependency_path)) != 0 ||
            compile_translation_unit(
                state,
                index,
                kind,
                ir_path,
                dependency_path) != 0) {
            return -1;
        }
        ++compiled_count;
    }

    state->last_recompiled_count = compiled_count;
    if (kind == CVITE_BUILD_CANDIDATE && compiled_count == 0U) {
        return 1;
    }
    return 0;
}

static int link_program(
    const cvite_run_state *state,
    cvite_build_kind kind,
    char linked_ir[PATH_MAX])
{
    char **arguments;
    char (*paths)[PATH_MAX] = NULL;
    size_t argument_count;
    size_t index;
    size_t argument_index = 0U;
    int status;

    if (cvite_build_path(state, "program.linked.ll", linked_ir) != 0 ||
        state->translation_unit_count > SIZE_MAX - 5U) {
        return -1;
    }
    argument_count = state->translation_unit_count + 5U;
    arguments = calloc(argument_count, sizeof(*arguments));
    paths = calloc(state->translation_unit_count, sizeof(*paths));
    if (arguments == NULL || paths == NULL) {
        free(arguments);
        free(paths);
        return -1;
    }

    arguments[argument_index++] = (char *)state->llvm_link_path;
    for (index = 0U; index < state->translation_unit_count; ++index) {
        const bool staged = kind == CVITE_BUILD_CANDIDATE &&
            state->candidate_dirty != NULL && state->candidate_dirty[index] != 0U;
        const int path_status = staged
            ? staged_ir_path(state, index, paths[index])
            : cvite_active_ir_path(state, index, paths[index]);

        if (path_status != 0) {
            free(arguments);
            free(paths);
            return -1;
        }
        arguments[argument_index++] = paths[index];
    }
    arguments[argument_index++] = (char *)"-S";
    arguments[argument_index++] = (char *)"-o";
    arguments[argument_index++] = linked_ir;
    arguments[argument_index] = NULL;

    cvite_remove_if_present(linked_ir);
    status = run_process(arguments);
    free(arguments);
    free(paths);
    return status == 0 ? 0 : -1;
}

static int transform_and_emit_program(
    const cvite_run_state *state,
    cvite_build_kind kind,
    const char *linked_ir,
    char object_path[PATH_MAX])
{
    char transformed_ir[PATH_MAX];
    char plugin_argument[PATH_MAX + 32U];
    const char *passes = kind == CVITE_BUILD_BASELINE
        ? "-passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,verify"
        : "-passes=cvite-lowering,cvite-candidate,verify";
    char *transform_arguments[8];
    char *emit_arguments[10];
    int length;
    int status;
    int result = -1;

    if (cvite_build_path(state, "program.transformed.ll", transformed_ir) != 0 ||
        cvite_build_path(state, "program.o", object_path) != 0) {
        return -1;
    }
    length = snprintf(
        plugin_argument,
        sizeof(plugin_argument),
        "-load-pass-plugin=%s",
        state->plugin_path);
    if (length < 0 || (size_t)length >= sizeof(plugin_argument)) {
        return -1;
    }

    transform_arguments[0] = (char *)state->opt_path;
    transform_arguments[1] = plugin_argument;
    transform_arguments[2] = (char *)passes;
    transform_arguments[3] = (char *)"-S";
    transform_arguments[4] = (char *)linked_ir;
    transform_arguments[5] = (char *)"-o";
    transform_arguments[6] = transformed_ir;
    transform_arguments[7] = NULL;

    emit_arguments[0] = (char *)state->clang_path;
    emit_arguments[1] = (char *)"-O0";
    emit_arguments[2] = (char *)"-g";
    emit_arguments[3] = (char *)"-fPIC";
    emit_arguments[4] = (char *)"-c";
    emit_arguments[5] = transformed_ir;
    emit_arguments[6] = (char *)"-o";
    emit_arguments[7] = object_path;
    emit_arguments[8] = NULL;
    emit_arguments[9] = NULL;

    cvite_remove_if_present(transformed_ir);
    cvite_remove_if_present(object_path);
    status = run_process(transform_arguments);
    if (status != 0) {
        report_build_failure(kind, "program transformation", NULL);
        goto cleanup;
    }
    status = run_process(emit_arguments);
    if (status != 0) {
        report_build_failure(kind, "object emission", NULL);
        goto cleanup;
    }
    result = 0;

cleanup:
    cvite_remove_if_present(transformed_ir);
    if (result != 0) {
        cvite_remove_if_present(object_path);
    }
    return result;
}

cvite_build_result cvite_compile_program(
    cvite_run_state *state,
    cvite_build_kind kind,
    char object_path[PATH_MAX])
{
    char linked_ir[PATH_MAX];
    cvite_project_result project_status;
    int compile_status;

    if (state == NULL || object_path == NULL) {
        return CVITE_BUILD_RESULT_FAILED;
    }
    if (kind == CVITE_BUILD_CANDIDATE) {
        cvite_discard_candidate_build(state);
    }

    project_status = cvite_refresh_project(state);
    if (project_status == CVITE_PROJECT_RESULT_RESTART_REQUIRED) {
        return CVITE_BUILD_RESULT_RESTART_REQUIRED;
    }
    if (project_status != CVITE_PROJECT_RESULT_OK) {
        return CVITE_BUILD_RESULT_FAILED;
    }

    compile_status = compile_selected_units(state, kind);
    if (compile_status > 0) {
        cvite_discard_candidate_build(state);
        return CVITE_BUILD_RESULT_UNCHANGED;
    }
    if (compile_status < 0) {
        if (kind == CVITE_BUILD_CANDIDATE) {
            cvite_discard_candidate_build(state);
        }
        return CVITE_BUILD_RESULT_FAILED;
    }

    if (link_program(state, kind, linked_ir) != 0) {
        report_build_failure(kind, "LLVM linking", NULL);
        if (kind == CVITE_BUILD_CANDIDATE) {
            cvite_discard_candidate_build(state);
        }
        return CVITE_BUILD_RESULT_FAILED;
    }
    if (transform_and_emit_program(
            state,
            kind,
            linked_ir,
            object_path) != 0) {
        cvite_remove_if_present(linked_ir);
        if (kind == CVITE_BUILD_CANDIDATE) {
            cvite_discard_candidate_build(state);
        }
        return CVITE_BUILD_RESULT_FAILED;
    }
    cvite_remove_if_present(linked_ir);

    (void)fprintf(
        stderr,
        "[cvite] %s %zu/%zu translation unit%s\n",
        kind == CVITE_BUILD_BASELINE ? "compiled" : "recompiled",
        state->last_recompiled_count,
        state->translation_unit_count,
        state->translation_unit_count == 1U ? "" : "s");
    return CVITE_BUILD_RESULT_OK;
}

void cvite_commit_candidate_build(cvite_run_state *state)
{
    size_t index;

    if (state == NULL || state->candidate_dirty == NULL) {
        return;
    }
    for (index = 0U; index < state->candidate_dirty_count; ++index) {
        char staged_ir[PATH_MAX];
        char staged_dependency[PATH_MAX];
        char active_ir[PATH_MAX];
        char active_dependency[PATH_MAX];

        if (state->candidate_dirty[index] == 0U) {
            continue;
        }
        if (staged_ir_path(state, index, staged_ir) != 0 ||
            staged_dependency_path(state, index, staged_dependency) != 0 ||
            cvite_active_ir_path(state, index, active_ir) != 0 ||
            cvite_active_dependency_path(state, index, active_dependency) != 0 ||
            rename(staged_dependency, active_dependency) != 0 ||
            rename(staged_ir, active_ir) != 0) {
            (void)fprintf(
                stderr,
                "[cvite] warning: could not commit TU cache %zu: %s\n",
                index,
                strerror(errno));
        }
    }
    free(state->candidate_dirty);
    state->candidate_dirty = NULL;
    state->candidate_dirty_count = 0U;
}

void cvite_discard_candidate_build(cvite_run_state *state)
{
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0U; index < state->translation_unit_count; ++index) {
        char path[PATH_MAX];

        if (staged_ir_path(state, index, path) == 0) {
            cvite_remove_if_present(path);
        }
        if (staged_dependency_path(state, index, path) == 0) {
            cvite_remove_if_present(path);
        }
    }
    free(state->candidate_dirty);
    state->candidate_dirty = NULL;
    state->candidate_dirty_count = 0U;
    state->last_recompiled_count = 0U;
}

void cvite_print_runtime_error(const char *stage, const cvite_error *error)
{
    const cvite_status status = error != NULL
        ? error->status
        : CVITE_STATUS_INVALID_STATE;
    const char *message = error != NULL && error->message[0] != '\0'
        ? error->message
        : "no diagnostic was provided";

    (void)fprintf(
        stderr,
        "[cvite] %s failed (%s): %s\n",
        stage,
        cvite_status_string(status),
        message);
}
