#include "run_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CVITE_COMPILE_DATABASE_ENABLED
#define CVITE_COMPILE_DATABASE_ENABLED 0
#endif

#if CVITE_COMPILE_DATABASE_ENABLED
#include <clang-c/CXCompilationDatabase.h>
#endif

#define CVITE_MAX_TRANSLATION_UNITS 4096U

#if CVITE_COMPILE_DATABASE_ENABLED
static char *duplicate_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }
    length = strlen(value);
    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = malloc(length + 1U);
    if (copy != NULL) {
        (void)memcpy(copy, value, length + 1U);
    }
    return copy;
}
#endif

static void free_arguments(char **arguments, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        free(arguments[index]);
    }
    free(arguments);
}

static void free_translation_units(cvite_translation_unit *units, size_t count)
{
    size_t index;

    if (units == NULL) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        free_arguments(
            units[index].compile_arguments,
            units[index].compile_argument_count);
    }
    free(units);
}

void cvite_free_project(cvite_run_state *state)
{
    if (state == NULL) {
        return;
    }
    free_translation_units(
        state->translation_units,
        state->translation_unit_count);
    state->translation_units = NULL;
    state->translation_unit_count = 0U;
    state->compile_database_path[0] = '\0';
}

static bool path_is_regular_file(const char *path)
{
    struct stat status;

    return path != NULL && stat(path, &status) == 0 && S_ISREG(status.st_mode);
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
        return 1;
    }
    *separator = '\0';
    return 0;
}

static bool string_ends_with(const char *value, const char *suffix)
{
    const size_t value_length = value != NULL ? strlen(value) : 0U;
    const size_t suffix_length = suffix != NULL ? strlen(suffix) : 0U;

    return suffix_length != 0U && value_length >= suffix_length &&
        strcmp(value + value_length - suffix_length, suffix) == 0;
}

#if CVITE_COMPILE_DATABASE_ENABLED
static bool path_is_within(const char *root, const char *path)
{
    const size_t root_length = root != NULL ? strlen(root) : 0U;

    if (root_length == 0U || path == NULL || strncmp(root, path, root_length) != 0) {
        return false;
    }
    if (root_length == 1U && root[0] == '/') {
        return true;
    }
    return path[root_length] == '\0' || path[root_length] == '/';
}

static char *make_absolute_path(const char *directory, const char *value)
{
    char joined[PATH_MAX];
    char resolved[PATH_MAX];

    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    if (value[0] == '/') {
        if (realpath(value, resolved) != NULL) {
            return duplicate_string(resolved);
        }
        return duplicate_string(value);
    }
    if (cvite_join_path(joined, sizeof(joined), directory, value) != 0) {
        return NULL;
    }
    if (realpath(joined, resolved) != NULL) {
        return duplicate_string(resolved);
    }
    return duplicate_string(joined);
}
#endif

static int compare_translation_units(const void *left, const void *right)
{
    const cvite_translation_unit *first = left;
    const cvite_translation_unit *second = right;

    return strcmp(first->source_path, second->source_path);
}

static int move_entry_translation_unit_first(
    cvite_translation_unit *units,
    size_t count,
    const char *entry_source)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(units[index].source_path, entry_source) == 0) {
            if (index != 0U) {
                const cvite_translation_unit entry = units[index];

                (void)memmove(
                    &units[1],
                    &units[0],
                    index * sizeof(units[0]));
                units[0] = entry;
            }
            return 0;
        }
    }
    return -1;
}

static int append_fallback_unit(
    cvite_translation_unit **units,
    size_t *count,
    size_t *capacity,
    const char *source_path,
    const char *working_directory)
{
    cvite_translation_unit *grown;
    size_t new_capacity;

    if (*count == *capacity) {
        new_capacity = *capacity == 0U ? 8U : *capacity * 2U;
        if (new_capacity < *capacity ||
            new_capacity > CVITE_MAX_TRANSLATION_UNITS ||
            new_capacity > SIZE_MAX / sizeof(**units)) {
            return -1;
        }
        grown = realloc(*units, new_capacity * sizeof(**units));
        if (grown == NULL) {
            return -1;
        }
        *units = grown;
        *capacity = new_capacity;
    }

    (void)memset(&(*units)[*count], 0, sizeof((*units)[*count]));
    if (copy_path((*units)[*count].source_path, source_path) != 0 ||
        copy_path(
            (*units)[*count].working_directory,
            working_directory) != 0) {
        return -1;
    }
    *count += 1U;
    return 0;
}

static int build_fallback_project(
    const cvite_run_state *state,
    cvite_translation_unit **units,
    size_t *unit_count)
{
    cvite_translation_unit *loaded = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    char working_directory[PATH_MAX];
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    DIR *directory = NULL;
    struct dirent *entry;
    bool contains_entry = false;
    int result = -1;

    if (copy_path(working_directory, state->source_path) != 0 ||
        parent_directory(working_directory) < 0) {
        return -1;
    }
    directory = opendir(working_directory);
    if (directory == NULL) {
        (void)fprintf(
            stderr,
            "[cvite] could not scan %s: %s\n",
            working_directory,
            strerror(errno));
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        if (!string_ends_with(entry->d_name, ".c") ||
            cvite_join_path(
                candidate,
                sizeof(candidate),
                working_directory,
                entry->d_name) != 0 ||
            realpath(candidate, resolved) == NULL ||
            !path_is_regular_file(resolved)) {
            continue;
        }
        if (count >= CVITE_MAX_TRANSLATION_UNITS ||
            append_fallback_unit(
                &loaded,
                &count,
                &capacity,
                resolved,
                working_directory) != 0) {
            (void)fprintf(
                stderr,
                "[cvite] fallback project exceeds the %u translation-unit limit\n",
                CVITE_MAX_TRANSLATION_UNITS);
            goto cleanup;
        }
        if (strcmp(resolved, state->source_path) == 0) {
            contains_entry = true;
        }
    }

    if (!contains_entry || count == 0U) {
        (void)fprintf(
            stderr,
            "[cvite] fallback source discovery lost the entry translation unit\n");
        goto cleanup;
    }
    qsort(loaded, count, sizeof(loaded[0]), compare_translation_units);
    if (move_entry_translation_unit_first(
            loaded, count, state->source_path) != 0) {
        goto cleanup;
    }
    *units = loaded;
    *unit_count = count;
    loaded = NULL;
    result = 0;

cleanup:
    (void)closedir(directory);
    free_translation_units(loaded, count);
    return result;
}

#if CVITE_COMPILE_DATABASE_ENABLED

typedef struct cvite_argument_vector {
    char **values;
    size_t count;
    size_t capacity;
} cvite_argument_vector;

typedef struct cvite_unit_vector {
    cvite_translation_unit *values;
    size_t count;
    size_t capacity;
} cvite_unit_vector;

typedef struct cvite_loaded_project {
    char database_path[PATH_MAX];
    cvite_unit_vector units;
} cvite_loaded_project;

static int reserve_arguments(cvite_argument_vector *vector, size_t required)
{
    size_t capacity;
    char **grown;

    if (required <= vector->capacity) {
        return 0;
    }
    capacity = vector->capacity == 0U ? 16U : vector->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return -1;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*grown)) {
        return -1;
    }
    grown = realloc(vector->values, capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    vector->values = grown;
    vector->capacity = capacity;
    return 0;
}

static int append_owned_argument(cvite_argument_vector *vector, char *value)
{
    if (value == NULL || reserve_arguments(vector, vector->count + 1U) != 0) {
        free(value);
        return -1;
    }
    vector->values[vector->count++] = value;
    return 0;
}

static int reserve_units(cvite_unit_vector *vector, size_t required)
{
    size_t capacity;
    cvite_translation_unit *grown;

    if (required <= vector->capacity) {
        return 0;
    }
    capacity = vector->capacity == 0U ? 8U : vector->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return -1;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*grown)) {
        return -1;
    }
    grown = realloc(vector->values, capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    vector->values = grown;
    vector->capacity = capacity;
    return 0;
}

static void free_loaded_project(cvite_loaded_project *project)
{
    if (project == NULL) {
        return;
    }
    free_translation_units(project->units.values, project->units.count);
    project->units.values = NULL;
    project->units.count = 0U;
    project->units.capacity = 0U;
}

static char *copy_cx_string(CXString value)
{
    const char *text = clang_getCString(value);
    char *copy = duplicate_string(text != NULL ? text : "");

    clang_disposeString(value);
    return copy;
}


static bool option_skips_next(const char *argument)
{
    return strcmp(argument, "-o") == 0 ||
        strcmp(argument, "--output") == 0 ||
        strcmp(argument, "-MF") == 0 ||
        strcmp(argument, "-MT") == 0 ||
        strcmp(argument, "-MQ") == 0 ||
        strcmp(argument, "-MJ") == 0 ||
        strcmp(argument, "-dependency-file") == 0 ||
        strcmp(argument, "--serialize-diagnostics") == 0 ||
        strcmp(argument, "-working-directory") == 0 ||
        strcmp(argument, "-Xlinker") == 0 ||
        strcmp(argument, "-L") == 0 || strcmp(argument, "-T") == 0 ||
        strcmp(argument, "-u") == 0 || strcmp(argument, "-z") == 0 ||
        strcmp(argument, "--script") == 0;
}
static bool option_takes_path(const char *argument)
{
    return strcmp(argument, "-I") == 0 ||
        strcmp(argument, "-isystem") == 0 ||
        strcmp(argument, "-iquote") == 0 ||
        strcmp(argument, "-idirafter") == 0 ||
        strcmp(argument, "-include") == 0 ||
        strcmp(argument, "-imacros") == 0 ||
        strcmp(argument, "-include-pch") == 0 ||
        strcmp(argument, "-isysroot") == 0 ||
        strcmp(argument, "--sysroot") == 0 ||
        strcmp(argument, "-resource-dir") == 0;
}

static bool is_optimization_flag(const char *argument)
{
    if (argument[0] != '-' || argument[1] != 'O') {
        return false;
    }
    return argument[2] == '\0' || argument[2] == '0' || argument[2] == '1' ||
        argument[2] == '2' || argument[2] == '3' || argument[2] == 's' ||
        argument[2] == 'z' || strncmp(argument, "-Ofast", 6U) == 0 ||
        strncmp(argument, "-Og", 3U) == 0;
}

static bool is_debug_flag(const char *argument)
{
    return strcmp(argument, "-g") == 0 || strcmp(argument, "-g0") == 0 ||
        strncmp(argument, "-gdwarf", 7U) == 0 ||
        strncmp(argument, "-ggdb", 5U) == 0 ||
        strcmp(argument, "-gline-tables-only") == 0 ||
        strcmp(argument, "-gmodules") == 0;
}


static bool option_is_controlled(const char *argument)
{
    return strcmp(argument, "--") == 0 || strcmp(argument, "-c") == 0 ||
        strcmp(argument, "-S") == 0 || strcmp(argument, "-E") == 0 ||
        strcmp(argument, "-emit-llvm") == 0 ||
        strcmp(argument, "-fsyntax-only") == 0 ||
        strcmp(argument, "-save-temps") == 0 ||
        strncmp(argument, "-save-temps=", 12U) == 0 ||
        strcmp(argument, "-MD") == 0 || strcmp(argument, "-MMD") == 0 ||
        strcmp(argument, "-M") == 0 || strcmp(argument, "-MM") == 0 ||
        strcmp(argument, "-MG") == 0 || strcmp(argument, "-MP") == 0 ||
        strncmp(argument, "--output=", 9U) == 0 ||
        (strncmp(argument, "-o", 2U) == 0 && argument[2] != '\0' &&
         strcmp(argument, "-objc") != 0 && strcmp(argument, "-objc++") != 0) ||
        strncmp(argument, "-MF", 3U) == 0 ||
        strncmp(argument, "-MT", 3U) == 0 ||
        strncmp(argument, "-MQ", 3U) == 0 ||
        strncmp(argument, "-MJ", 3U) == 0 ||
        strncmp(argument, "-dependency-file=", 17U) == 0 ||
        strncmp(argument, "--serialize-diagnostics=", 24U) == 0 ||
        strncmp(argument, "-Wp,-M", 6U) == 0 ||
        strcmp(argument, "-fPIC") == 0 || strcmp(argument, "-fpic") == 0 ||
        strcmp(argument, "-fPIE") == 0 || strcmp(argument, "-fpie") == 0 ||
        strcmp(argument, "-fno-inline") == 0 ||
        strcmp(argument, "-fomit-frame-pointer") == 0 ||
        strcmp(argument, "-fno-omit-frame-pointer") == 0 ||
        strcmp(argument, "-fdiscard-value-names") == 0 ||
        strcmp(argument, "-fno-discard-value-names") == 0 ||
        strcmp(argument, "-shared") == 0 || strcmp(argument, "-static") == 0 ||
        strcmp(argument, "-pie") == 0 || strcmp(argument, "-rdynamic") == 0 ||
        strcmp(argument, "-nostdlib") == 0 ||
        strcmp(argument, "-nodefaultlibs") == 0 ||
        strcmp(argument, "-nostartfiles") == 0 || strcmp(argument, "-r") == 0 ||
        strcmp(argument, "-s") == 0 || strncmp(argument, "-Wl,", 4U) == 0 ||
        strncmp(argument, "-fuse-ld=", 10U) == 0 ||
        (strncmp(argument, "-l", 2U) == 0 && argument[2] != '\0') ||
        (strncmp(argument, "-L", 2U) == 0 && argument[2] != '\0') ||
        is_optimization_flag(argument) || is_debug_flag(argument);
}
static char *rewrite_joined_path(
    const char *directory,
    const char *argument,
    const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    const char *value = argument + prefix_length;
    char *absolute = make_absolute_path(directory, value);
    size_t absolute_length;
    char *rewritten;

    if (absolute == NULL) {
        return NULL;
    }
    absolute_length = strlen(absolute);
    if (prefix_length > SIZE_MAX - absolute_length - 1U) {
        free(absolute);
        return NULL;
    }
    rewritten = malloc(prefix_length + absolute_length + 1U);
    if (rewritten != NULL) {
        (void)memcpy(rewritten, prefix, prefix_length);
        (void)memcpy(
            rewritten + prefix_length,
            absolute,
            absolute_length + 1U);
    }
    free(absolute);
    return rewritten;
}

static char *rewrite_path_argument(
    const char *directory,
    const char *argument)
{
    static const char *const joined_prefixes[] = {
        "--sysroot=",
        "-fmodule-map-file=",
        "-I",
        "-isystem",
        "-iquote",
        "-idirafter",
        "-include-pch",
        "-include",
        "-imacros",
        "-isysroot",
        "-resource-dir=",
    };
    size_t index;

    if (argument[0] == '@' && argument[1] != '\0') {
        char *absolute = make_absolute_path(directory, argument + 1);
        size_t length;
        char *rewritten;

        if (absolute == NULL) {
            return NULL;
        }
        length = strlen(absolute);
        if (length == SIZE_MAX) {
            free(absolute);
            return NULL;
        }
        rewritten = malloc(length + 2U);
        if (rewritten != NULL) {
            rewritten[0] = '@';
            (void)memcpy(rewritten + 1U, absolute, length + 1U);
        }
        free(absolute);
        return rewritten;
    }

    for (index = 0U;
         index < sizeof(joined_prefixes) / sizeof(joined_prefixes[0]);
         ++index) {
        const char *prefix = joined_prefixes[index];
        const size_t length = strlen(prefix);

        if (strncmp(argument, prefix, length) == 0 &&
            argument[length] != '\0') {
            return rewrite_joined_path(directory, argument, prefix);
        }
    }
    return duplicate_string(argument);
}

static bool argument_names_source(
    const char *directory,
    const char *argument,
    const char *source_path)
{
    char *absolute;
    bool matches;

    if (argument == NULL || argument[0] == '-' || argument[0] == '\0') {
        return false;
    }
    absolute = make_absolute_path(directory, argument);
    if (absolute == NULL) {
        return false;
    }
    matches = strcmp(absolute, source_path) == 0;
    free(absolute);
    return matches;
}

static int load_arguments(
    CXCompileCommand command,
    const char *directory,
    const char *source_path,
    cvite_argument_vector *arguments)
{
    const unsigned argument_count = clang_CompileCommand_getNumArgs(command);
    unsigned index;

    for (index = 1U; index < argument_count; ++index) {
        char *argument = copy_cx_string(
            clang_CompileCommand_getArg(command, index));

        if (argument == NULL) {
            return -1;
        }
        if (option_skips_next(argument)) {
            free(argument);
            if (index + 1U >= argument_count) {
                (void)fprintf(
                    stderr,
                    "[cvite] malformed compile command option without value\n");
                return -1;
            }
            ++index;
            continue;
        }
        if (strcmp(argument, "-Xclang") == 0 && index + 1U < argument_count) {
            char *next = copy_cx_string(
                clang_CompileCommand_getArg(command, index + 1U));
            if (next == NULL) {
                free(argument);
                return -1;
            }
            if (strcmp(next, "-disable-O0-optnone") == 0) {
                free(argument);
                free(next);
                ++index;
                continue;
            }
            if (append_owned_argument(arguments, argument) != 0 ||
                append_owned_argument(arguments, next) != 0) {
                return -1;
            }
            ++index;
            continue;
        }
        if (option_takes_path(argument)) {
            char *path;
            char *absolute;

            if (index + 1U >= argument_count) {
                free(argument);
                (void)fprintf(
                    stderr,
                    "[cvite] malformed path option in compile command\n");
                return -1;
            }
            path = copy_cx_string(
                clang_CompileCommand_getArg(command, index + 1U));
            if (path == NULL) {
                free(argument);
                return -1;
            }
            absolute = make_absolute_path(directory, path);
            free(path);
            if (absolute == NULL ||
                append_owned_argument(arguments, argument) != 0 ||
                append_owned_argument(arguments, absolute) != 0) {
                return -1;
            }
            ++index;
            continue;
        }
        if (option_is_controlled(argument) ||
            argument_names_source(directory, argument, source_path)) {
            free(argument);
            continue;
        }
        {
            char *rewritten = rewrite_path_argument(directory, argument);
            free(argument);
            if (append_owned_argument(arguments, rewritten) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int command_paths(
    CXCompileCommand command,
    const char *database_directory,
    char working_directory[PATH_MAX],
    char source_path[PATH_MAX])
{
    char *directory = copy_cx_string(
        clang_CompileCommand_getDirectory(command));
    char *filename = copy_cx_string(
        clang_CompileCommand_getFilename(command));
    char *absolute_directory = NULL;
    char *absolute_source = NULL;
    int result = -1;

    if (directory == NULL || filename == NULL || directory[0] == '\0' ||
        filename[0] == '\0') {
        goto cleanup;
    }
    absolute_directory = make_absolute_path(database_directory, directory);
    if (absolute_directory == NULL ||
        realpath(absolute_directory, working_directory) == NULL) {
        goto cleanup;
    }
    absolute_source = make_absolute_path(working_directory, filename);
    if (absolute_source == NULL ||
        copy_path(source_path, absolute_source) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    free(directory);
    free(filename);
    free(absolute_directory);
    free(absolute_source);
    return result;
}

static bool unit_exists(
    const cvite_unit_vector *units,
    const char *source_path)
{
    size_t index;

    for (index = 0U; index < units->count; ++index) {
        if (strcmp(units->values[index].source_path, source_path) == 0) {
            return true;
        }
    }
    return false;
}

static int append_command_unit(
    cvite_loaded_project *project,
    CXCompileCommand command,
    const char *database_directory,
    const char *project_root)
{
    cvite_translation_unit unit = {0};
    cvite_argument_vector arguments = {0};

    if (command_paths(
            command,
            database_directory,
            unit.working_directory,
            unit.source_path) != 0) {
        return -1;
    }
    if (!path_is_regular_file(unit.source_path) ||
        !string_ends_with(unit.source_path, ".c") ||
        !path_is_within(project_root, unit.source_path)) {
        return 0;
    }
    if (unit_exists(&project->units, unit.source_path)) {
        return 0;
    }
    if (project->units.count >= CVITE_MAX_TRANSLATION_UNITS) {
        (void)fprintf(
            stderr,
            "[cvite] compile database exceeds the %u translation-unit limit\n",
            CVITE_MAX_TRANSLATION_UNITS);
        return -1;
    }
    if (load_arguments(
            command,
            unit.working_directory,
            unit.source_path,
            &arguments) != 0) {
        free_arguments(arguments.values, arguments.count);
        return -1;
    }
    unit.compile_arguments = arguments.values;
    unit.compile_argument_count = arguments.count;
    if (reserve_units(&project->units, project->units.count + 1U) != 0) {
        free_arguments(unit.compile_arguments, unit.compile_argument_count);
        return -1;
    }
    project->units.values[project->units.count++] = unit;
    return 0;
}

static bool project_contains_entry(
    const cvite_loaded_project *project,
    const char *entry_source)
{
    size_t index;

    for (index = 0U; index < project->units.count; ++index) {
        if (strcmp(project->units.values[index].source_path, entry_source) == 0) {
            return true;
        }
    }
    return false;
}

static int load_database_project(
    const cvite_run_state *state,
    const char *database_path,
    cvite_loaded_project *loaded)
{
    char database_directory[PATH_MAX];
    char resolved_database[PATH_MAX];
    char resolved_directory[PATH_MAX];
    CXCompilationDatabase_Error database_error;
    CXCompilationDatabase database = NULL;
    CXCompileCommands commands = NULL;
    unsigned count;
    unsigned index;
    int result = -1;

    if (copy_path(database_directory, database_path) != 0 ||
        parent_directory(database_directory) < 0 ||
        realpath(database_path, resolved_database) == NULL ||
        realpath(database_directory, resolved_directory) == NULL) {
        (void)fprintf(
            stderr,
            "[cvite] could not resolve compile database %s: %s\n",
            database_path,
            strerror(errno));
        return -1;
    }

    database = clang_CompilationDatabase_fromDirectory(
        resolved_directory,
        &database_error);
    if (database == NULL || database_error != CXCompilationDatabase_NoError) {
        (void)fprintf(
            stderr,
            "[cvite] could not load %s with libclang\n",
            resolved_database);
        goto cleanup;
    }
    commands = clang_CompilationDatabase_getAllCompileCommands(database);
    count = clang_CompileCommands_getSize(commands);
    for (index = 0U; index < count; ++index) {
        if (append_command_unit(
                loaded,
                clang_CompileCommands_getCommand(commands, index),
                resolved_directory,
                state->project_root) != 0) {
            goto cleanup;
        }
    }
    if (loaded->units.count == 0U) {
        result = 1;
        goto cleanup;
    }
    if (!project_contains_entry(loaded, state->source_path)) {
        result = 1;
        goto cleanup;
    }
    qsort(
        loaded->units.values,
        loaded->units.count,
        sizeof(loaded->units.values[0]),
        compare_translation_units);
    if (move_entry_translation_unit_first(
            loaded->units.values,
            loaded->units.count,
            state->source_path) != 0 ||
        copy_path(loaded->database_path, resolved_database) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (commands != NULL) {
        clang_CompileCommands_dispose(commands);
    }
    if (database != NULL) {
        clang_CompilationDatabase_dispose(database);
    }
    if (result != 0) {
        free_loaded_project(loaded);
    }
    return result;
}

static int search_compile_database(
    const cvite_run_state *state,
    cvite_loaded_project *loaded)
{
    char current[PATH_MAX];
    char candidate[PATH_MAX];
    char build_directory[PATH_MAX];
    const char *override = getenv("CVITE_COMPILE_COMMANDS");

    if (override != NULL && override[0] != '\0') {
        if (!path_is_regular_file(override)) {
            (void)fprintf(
                stderr,
                "[cvite] CVITE_COMPILE_COMMANDS is not a readable file: %s\n",
                override);
            return -1;
        }
        {
            const int status = load_database_project(state, override, loaded);

            if (status > 0) {
                (void)fprintf(
                    stderr,
                    "[cvite] CVITE_COMPILE_COMMANDS does not describe the "
                    "entry translation unit: %s\n",
                    state->source_path);
                return -1;
            }
            return status;
        }
    }

    if (copy_path(current, state->project_root) != 0) {
        return -1;
    }
    for (;;) {
        int status;

        if (cvite_join_path(
                candidate,
                sizeof(candidate),
                current,
                "compile_commands.json") != 0) {
            return -1;
        }
        if (path_is_regular_file(candidate)) {
            status = load_database_project(state, candidate, loaded);
            if (status <= 0) {
                return status;
            }
        }
        if (cvite_join_path(
                build_directory,
                sizeof(build_directory),
                current,
                "build") != 0 ||
            cvite_join_path(
                candidate,
                sizeof(candidate),
                build_directory,
                "compile_commands.json") != 0) {
            return -1;
        }
        if (path_is_regular_file(candidate)) {
            status = load_database_project(state, candidate, loaded);
            if (status <= 0) {
                return status;
            }
        }
        status = parent_directory(current);
        if (status != 0) {
            break;
        }
    }
    return 1;
}

static bool arguments_equal(
    const cvite_translation_unit *left,
    const cvite_translation_unit *right)
{
    size_t index;

    if (left->compile_argument_count != right->compile_argument_count) {
        return false;
    }
    for (index = 0U; index < left->compile_argument_count; ++index) {
        if (strcmp(
                left->compile_arguments[index],
                right->compile_arguments[index]) != 0) {
            return false;
        }
    }
    return true;
}

static bool project_equal(
    const cvite_run_state *state,
    const cvite_loaded_project *loaded)
{
    size_t index;

    if (strcmp(state->compile_database_path, loaded->database_path) != 0 ||
        state->translation_unit_count != loaded->units.count) {
        return false;
    }
    for (index = 0U; index < loaded->units.count; ++index) {
        const cvite_translation_unit *old_unit = &state->translation_units[index];
        const cvite_translation_unit *new_unit = &loaded->units.values[index];
        if (strcmp(old_unit->source_path, new_unit->source_path) != 0 ||
            strcmp(
                old_unit->working_directory,
                new_unit->working_directory) != 0 ||
            !arguments_equal(old_unit, new_unit)) {
            return false;
        }
    }
    return true;
}

cvite_project_result cvite_refresh_project(cvite_run_state *state)
{
    cvite_loaded_project loaded = {0};
    cvite_translation_unit *fallback_units = NULL;
    size_t fallback_count = 0U;
    int status;

    if (state == NULL) {
        return CVITE_PROJECT_RESULT_FAILED;
    }
    status = search_compile_database(state, &loaded);
    if (status > 0) {
        if (state->compile_database_path[0] != '\0') {
            (void)fprintf(
                stderr,
                "[cvite] active compile_commands.json disappeared or no longer "
                "describes the entry program; restart required\n");
            return CVITE_PROJECT_RESULT_RESTART_REQUIRED;
        }
        if (build_fallback_project(state, &fallback_units, &fallback_count) != 0) {
            return CVITE_PROJECT_RESULT_FAILED;
        }
        loaded.units.values = fallback_units;
        loaded.units.count = fallback_count;
        loaded.units.capacity = fallback_count;

        if (state->translation_unit_count != 0U) {
            if (project_equal(state, &loaded)) {
                free_loaded_project(&loaded);
                return CVITE_PROJECT_RESULT_OK;
            }
            free_loaded_project(&loaded);
            (void)fprintf(
                stderr,
                "[cvite] the translation-unit set changed; restart required\n");
            return CVITE_PROJECT_RESULT_RESTART_REQUIRED;
        }

        state->translation_units = loaded.units.values;
        state->translation_unit_count = loaded.units.count;
        loaded.units.values = NULL;
        loaded.units.count = 0U;
        loaded.units.capacity = 0U;
        (void)fprintf(
            stderr,
            "[cvite] no compile_commands.json; discovered %zu sibling "
            "translation unit%s\n",
            fallback_count,
            fallback_count == 1U ? "" : "s");
        return CVITE_PROJECT_RESULT_OK;
    }
    if (status < 0) {
        free_loaded_project(&loaded);
        return CVITE_PROJECT_RESULT_FAILED;
    }

    if (state->translation_unit_count != 0U) {
        if (project_equal(state, &loaded)) {
            free_loaded_project(&loaded);
            return CVITE_PROJECT_RESULT_OK;
        }
        free_loaded_project(&loaded);
        (void)fprintf(
            stderr,
            "[cvite] compile commands or translation-unit membership changed; "
            "restart required\n");
        return CVITE_PROJECT_RESULT_RESTART_REQUIRED;
    }

    state->translation_units = loaded.units.values;
    state->translation_unit_count = loaded.units.count;
    loaded.units.values = NULL;
    loaded.units.count = 0U;
    loaded.units.capacity = 0U;
    (void)copy_path(state->compile_database_path, loaded.database_path);

    {
        size_t flag_count = 0U;
        size_t index;

        for (index = 0U; index < state->translation_unit_count; ++index) {
            flag_count += state->translation_units[index].compile_argument_count;
        }
        (void)fprintf(
            stderr,
            "[cvite] project: %s (%zu translation unit%s, %zu normalized flag%s)\n",
            state->compile_database_path,
            state->translation_unit_count,
            state->translation_unit_count == 1U ? "" : "s",
            flag_count,
            flag_count == 1U ? "" : "s");
    }
    return CVITE_PROJECT_RESULT_OK;
}

void cvite_compile_database_print_doctor(FILE *stream)
{
    if (stream != NULL) {
        (void)fprintf(
            stream,
            "compile database: libclang multi-TU compile_commands.json ingestion\n");
    }
}

#else

cvite_project_result cvite_refresh_project(cvite_run_state *state)
{
    cvite_translation_unit *units = NULL;
    size_t unit_count = 0U;

    if (state == NULL) {
        return CVITE_PROJECT_RESULT_FAILED;
    }
    if (state->translation_unit_count != 0U) {
        return CVITE_PROJECT_RESULT_OK;
    }
    if (build_fallback_project(state, &units, &unit_count) != 0) {
        return CVITE_PROJECT_RESULT_FAILED;
    }
    state->translation_units = units;
    state->translation_unit_count = unit_count;
    (void)fprintf(
        stderr,
        "[cvite] compile_commands.json ingestion is unavailable; "
        "using sibling translation-unit discovery\n");
    return CVITE_PROJECT_RESULT_OK;
}

void cvite_compile_database_print_doctor(FILE *stream)
{
    if (stream != NULL) {
        (void)fprintf(stream, "compile database: unavailable in this build\n");
    }
}

#endif
