#ifndef CVITE_CLI_RUN_INTERNAL_H
#define CVITE_CLI_RUN_INTERNAL_H

#include "cvite/orc_loader.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef enum cvite_build_kind {
    CVITE_BUILD_BASELINE = 0,
    CVITE_BUILD_CANDIDATE = 1
} cvite_build_kind;

typedef enum cvite_build_result {
    CVITE_BUILD_RESULT_FAILED = -1,
    CVITE_BUILD_RESULT_OK = 0,
    CVITE_BUILD_RESULT_UNCHANGED = 1,
    CVITE_BUILD_RESULT_RESTART_REQUIRED = 2
} cvite_build_result;

typedef enum cvite_project_result {
    CVITE_PROJECT_RESULT_FAILED = -1,
    CVITE_PROJECT_RESULT_OK = 0,
    CVITE_PROJECT_RESULT_RESTART_REQUIRED = 1
} cvite_project_result;

typedef struct cvite_translation_unit {
    char source_path[PATH_MAX];
    char working_directory[PATH_MAX];
    char **compile_arguments;
    size_t compile_argument_count;
} cvite_translation_unit;

typedef struct cvite_watch_directory {
    int handle;
    char *path;
    char **file_names;
    size_t file_count;
    size_t file_capacity;
} cvite_watch_directory;

typedef struct cvite_run_state {
    char source_path[PATH_MAX];
    char project_root[PATH_MAX];
    char compile_database_path[PATH_MAX];
    char build_directory[PATH_MAX];
    const char *clang_path;
    const char *opt_path;
    const char *llvm_link_path;
    const char *plugin_path;
    cvite_translation_unit *translation_units;
    size_t translation_unit_count;
    unsigned char *candidate_dirty;
    size_t candidate_dirty_count;
    size_t last_recompiled_count;
    cvite_orc_loader *loader;
    int watch_descriptor;
    cvite_watch_directory *watch_directories;
    size_t watch_directory_count;
    atomic_bool stop_requested;
} cvite_run_state;

int cvite_join_path(
    char *output,
    size_t output_size,
    const char *left,
    const char *right);

int cvite_build_path(
    const cvite_run_state *state,
    const char *name,
    char output[PATH_MAX]);

void cvite_remove_if_present(const char *path);

typedef int (*cvite_dependency_visitor)(const char *path, void *context);

int cvite_visit_dependency_file(
    const char *dependency_path,
    cvite_dependency_visitor visitor,
    void *context);

int cvite_active_ir_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX]);

int cvite_active_dependency_path(
    const cvite_run_state *state,
    size_t unit_index,
    char output[PATH_MAX]);

cvite_project_result cvite_refresh_project(cvite_run_state *state);
void cvite_free_project(cvite_run_state *state);
void cvite_compile_database_print_doctor(FILE *stream);

cvite_build_result cvite_compile_program(
    cvite_run_state *state,
    cvite_build_kind kind,
    char object_path[PATH_MAX]);

void cvite_commit_candidate_build(cvite_run_state *state);
void cvite_discard_candidate_build(cvite_run_state *state);

void cvite_print_runtime_error(const char *stage, const cvite_error *error);

int cvite_initialize_source_watcher(cvite_run_state *state);
int cvite_refresh_source_watcher(cvite_run_state *state);
void cvite_close_source_watcher(cvite_run_state *state);
void *cvite_watch_source(void *opaque);

#endif
