#include "run_internal.h"

#include "cvite/status.h"

#include <errno.h>
#include <stdbool.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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
    return cvite_join_path(output, PATH_MAX, state->build_directory, name);
}

int cvite_dependency_path(
    const cvite_run_state *state,
    cvite_build_kind kind,
    char output[PATH_MAX])
{
    return cvite_build_path(
        state,
        kind == CVITE_BUILD_BASELINE ? "baseline.d" : "candidate.d",
        output);
}

void cvite_remove_if_present(const char *path)
{
    if (path != NULL) {
        (void)unlink(path);
    }
}

static void report_build_failure(
    cvite_build_kind kind,
    const char *phase)
{
    (void)fprintf(
        stderr,
        "[cvite] %s %s failed%s\n",
        kind == CVITE_BUILD_BASELINE ? "baseline" : "candidate",
        phase,
        kind == CVITE_BUILD_BASELINE
            ? ""
            : "; previous code remains active");
}

int cvite_compile_translation_unit(
    const cvite_run_state *state,
    cvite_build_kind kind,
    char object_path[PATH_MAX])
{
    char raw_ir_path[PATH_MAX];
    char transformed_ir_path[PATH_MAX];
    char dependency_path[PATH_MAX];
    char plugin_argument[PATH_MAX + 32U];
    const char *passes = kind == CVITE_BUILD_BASELINE
        ? "-passes=cvite-lowering,cvite-baseline,cvite-baseline-manifest,verify"
        : "-passes=cvite-lowering,cvite-candidate,verify";
    int length;
    int status;
    char *clang_to_ir[] = {
        (char *)state->clang_path,
        (char *)"-std=c11",
        (char *)"-O0",
        (char *)"-g",
        (char *)"-fPIC",
        (char *)"-fno-inline",
        (char *)"-fno-omit-frame-pointer",
        (char *)"-fno-discard-value-names",
        (char *)"-Xclang",
        (char *)"-disable-O0-optnone",
        (char *)"-MMD",
        (char *)"-MF",
        dependency_path,
        (char *)"-MT",
        (char *)"cvite-translation",
        (char *)"-S",
        (char *)"-emit-llvm",
        (char *)state->source_path,
        (char *)"-o",
        raw_ir_path,
        NULL,
    };
    char *transform[] = {
        (char *)state->opt_path,
        plugin_argument,
        (char *)passes,
        (char *)"-S",
        raw_ir_path,
        (char *)"-o",
        transformed_ir_path,
        NULL,
    };
    char *emit_object[] = {
        (char *)state->clang_path,
        (char *)"-O0",
        (char *)"-g",
        (char *)"-fPIC",
        (char *)"-c",
        transformed_ir_path,
        (char *)"-o",
        object_path,
        NULL,
    };

    if (cvite_build_path(state, "translation.raw.ll", raw_ir_path) != 0 ||
        cvite_build_path(state, "translation.ll", transformed_ir_path) != 0 ||
        cvite_build_path(state, "translation.o", object_path) != 0 ||
        cvite_dependency_path(state, kind, dependency_path) != 0) {
        (void)fprintf(stderr, "[cvite] temporary build path is too long\n");
        return -1;
    }

    length = snprintf(
        plugin_argument,
        sizeof(plugin_argument),
        "-load-pass-plugin=%s",
        state->plugin_path);
    if (length < 0 || (size_t)length >= sizeof(plugin_argument)) {
        (void)fprintf(stderr, "[cvite] pass-plugin path is too long\n");
        return -1;
    }

    cvite_remove_if_present(raw_ir_path);
    cvite_remove_if_present(transformed_ir_path);
    cvite_remove_if_present(object_path);
    cvite_remove_if_present(dependency_path);

    status = run_process(clang_to_ir);
    if (status != 0) {
        report_build_failure(kind, "compilation");
        goto cleanup_failure;
    }
    status = run_process(transform);
    if (status != 0) {
        report_build_failure(kind, "transformation");
        goto cleanup_failure;
    }
    status = run_process(emit_object);
    if (status != 0) {
        report_build_failure(kind, "object emission");
        goto cleanup_failure;
    }

    cvite_remove_if_present(raw_ir_path);
    cvite_remove_if_present(transformed_ir_path);
    return 0;

cleanup_failure:
    cvite_remove_if_present(raw_ir_path);
    cvite_remove_if_present(transformed_ir_path);
    cvite_remove_if_present(object_path);
    cvite_remove_if_present(dependency_path);
    return -1;
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
