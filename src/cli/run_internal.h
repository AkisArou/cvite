#ifndef CVITE_CLI_RUN_INTERNAL_H
#define CVITE_CLI_RUN_INTERNAL_H

#include "cvite/orc_loader.h"

#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef enum cvite_build_kind {
    CVITE_BUILD_BASELINE = 0,
    CVITE_BUILD_CANDIDATE = 1
} cvite_build_kind;

typedef struct cvite_watch_directory {
    int handle;
    char *path;
    char **file_names;
    size_t file_count;
    size_t file_capacity;
} cvite_watch_directory;

typedef struct cvite_run_state {
    char source_path[PATH_MAX];
    char watch_directory[PATH_MAX];
    char watch_name[NAME_MAX + 1U];
    char build_directory[PATH_MAX];
    const char *clang_path;
    const char *opt_path;
    const char *plugin_path;
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

int cvite_dependency_path(
    const cvite_run_state *state,
    cvite_build_kind kind,
    char output[PATH_MAX]);

void cvite_remove_if_present(const char *path);

int cvite_compile_translation_unit(
    const cvite_run_state *state,
    cvite_build_kind kind,
    char object_path[PATH_MAX]);

void cvite_print_runtime_error(const char *stage, const cvite_error *error);

int cvite_initialize_source_watcher(cvite_run_state *state);
int cvite_refresh_source_watcher(
    cvite_run_state *state,
    cvite_build_kind kind);
void cvite_close_source_watcher(cvite_run_state *state);
void *cvite_watch_source(void *opaque);

#endif
