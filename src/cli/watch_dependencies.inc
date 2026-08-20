typedef struct cvite_watch_set {
    cvite_watch_directory *directories;
    size_t count;
    size_t capacity;
} cvite_watch_set;

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

static void free_watch_directory(cvite_watch_directory *directory)
{
    size_t index;

    if (directory == NULL) {
        return;
    }
    for (index = 0U; index < directory->file_count; ++index) {
        free(directory->file_names[index]);
    }
    free(directory->file_names);
    free(directory->path);
    directory->handle = -1;
    directory->path = NULL;
    directory->file_names = NULL;
    directory->file_count = 0U;
    directory->file_capacity = 0U;
}

static void free_watch_directories(
    cvite_watch_directory *directories,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        free_watch_directory(&directories[index]);
    }
    free(directories);
}

static void free_watch_set(cvite_watch_set *set)
{
    if (set == NULL) {
        return;
    }
    free_watch_directories(set->directories, set->count);
    set->directories = NULL;
    set->count = 0U;
    set->capacity = 0U;
}

static cvite_watch_directory *find_directory_by_path(
    cvite_watch_directory *directories,
    size_t count,
    const char *path)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(directories[index].path, path) == 0) {
            return &directories[index];
        }
    }
    return NULL;
}

static const cvite_watch_directory *find_const_directory_by_path(
    const cvite_watch_directory *directories,
    size_t count,
    const char *path)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(directories[index].path, path) == 0) {
            return &directories[index];
        }
    }
    return NULL;
}

static const cvite_watch_directory *find_directory_by_handle(
    const cvite_run_state *state,
    int handle)
{
    size_t index;

    for (index = 0U; index < state->watch_directory_count; ++index) {
        if (state->watch_directories[index].handle == handle) {
            return &state->watch_directories[index];
        }
    }
    return NULL;
}

static bool directory_contains_file(
    const cvite_watch_directory *directory,
    const char *file_name)
{
    size_t index;

    for (index = 0U; index < directory->file_count; ++index) {
        if (strcmp(directory->file_names[index], file_name) == 0) {
            return true;
        }
    }
    return false;
}

static int reserve_directories(cvite_watch_set *set, size_t required)
{
    size_t capacity;
    cvite_watch_directory *grown;

    if (required <= set->capacity) {
        return 0;
    }
    capacity = set->capacity == 0U
        ? CVITE_INITIAL_DIRECTORY_CAPACITY
        : set->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return -1;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*grown)) {
        return -1;
    }
    grown = realloc(set->directories, capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    (void)memset(
        grown + set->capacity,
        0,
        (capacity - set->capacity) * sizeof(*grown));
    set->directories = grown;
    set->capacity = capacity;
    return 0;
}

static int reserve_files(cvite_watch_directory *directory, size_t required)
{
    size_t capacity;
    char **grown;

    if (required <= directory->file_capacity) {
        return 0;
    }
    capacity = directory->file_capacity == 0U
        ? CVITE_INITIAL_FILE_CAPACITY
        : directory->file_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return -1;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*grown)) {
        return -1;
    }
    grown = realloc(directory->file_names, capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    directory->file_names = grown;
    directory->file_capacity = capacity;
    return 0;
}

static int split_absolute_path(
    const char *path,
    char directory[PATH_MAX],
    char file_name[NAME_MAX + 1U])
{
    char *separator;
    size_t path_length;
    size_t file_length;

    if (path == NULL || path[0] != '/') {
        return -1;
    }
    path_length = strlen(path);
    if (path_length == 0U || path_length >= PATH_MAX) {
        return -1;
    }
    (void)memcpy(directory, path, path_length + 1U);
    separator = strrchr(directory, '/');
    if (separator == NULL || separator[1] == '\0') {
        return -1;
    }
    file_length = strlen(separator + 1);
    if (file_length == 0U || file_length > NAME_MAX) {
        return -1;
    }
    (void)memcpy(file_name, separator + 1, file_length + 1U);
    if (separator == directory) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    return 0;
}

static int watch_set_add_path(cvite_watch_set *set, const char *absolute_path)
{
    char directory_path[PATH_MAX];
    char file_name[NAME_MAX + 1U];
    cvite_watch_directory *directory;
    char *copy;

    if (split_absolute_path(absolute_path, directory_path, file_name) != 0) {
        (void)fprintf(
            stderr,
            "[cvite] dependency path cannot be watched: %s\n",
            absolute_path != NULL ? absolute_path : "(null)");
        return -1;
    }

    directory = find_directory_by_path(
        set->directories,
        set->count,
        directory_path);
    if (directory == NULL) {
        if (reserve_directories(set, set->count + 1U) != 0) {
            return -1;
        }
        directory = &set->directories[set->count];
        directory->handle = -1;
        directory->path = duplicate_string(directory_path);
        if (directory->path == NULL) {
            return -1;
        }
        ++set->count;
    }

    if (directory_contains_file(directory, file_name)) {
        return 0;
    }
    if (reserve_files(directory, directory->file_count + 1U) != 0) {
        return -1;
    }
    copy = duplicate_string(file_name);
    if (copy == NULL) {
        return -1;
    }
    directory->file_names[directory->file_count] = copy;
    ++directory->file_count;
    return 0;
}

static int read_file(
    const char *path,
    char **content,
    size_t *content_size)
{
    FILE *file;
    long length;
    size_t size;
    char *buffer;

    *content = NULL;
    *content_size = 0U;
    file = fopen(path, "rb");
    if (file == NULL) {
        (void)fprintf(
            stderr,
            "[cvite] could not read dependency file %s: %s\n",
            path,
            strerror(errno));
        return -1;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return -1;
    }
    length = ftell(file);
    if (length < 0L || (unsigned long)length > CVITE_MAX_DEPFILE_SIZE) {
        (void)fclose(file);
        (void)fprintf(stderr, "[cvite] dependency file is unreasonably large\n");
        return -1;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return -1;
    }
    size = (size_t)length;
    if (size == SIZE_MAX) {
        (void)fclose(file);
        return -1;
    }
    buffer = malloc(size + 1U);
    if (buffer == NULL) {
        (void)fclose(file);
        return -1;
    }
    if (size != 0U && fread(buffer, 1U, size, file) != size) {
        free(buffer);
        (void)fclose(file);
        (void)fprintf(stderr, "[cvite] could not read complete dependency file\n");
        return -1;
    }
    buffer[size] = '\0';
    (void)fclose(file);
    *content = buffer;
    *content_size = size;
    return 0;
}

static size_t dependency_list_offset(const char *content, size_t size)
{
    bool escaped = false;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (escaped) {
            escaped = false;
        } else if (content[index] == '\\') {
            escaped = true;
        } else if (content[index] == ':') {
            return index + 1U;
        }
    }
    return SIZE_MAX;
}

static int add_dependency_token(
    cvite_watch_set *set,
    const char *token,
    size_t token_length)
{
    char path[PATH_MAX];
    char resolved[PATH_MAX];

    if (token_length == 0U) {
        return 0;
    }
    if (token_length >= sizeof(path)) {
        (void)fprintf(stderr, "[cvite] dependency path is too long\n");
        return -1;
    }
    (void)memcpy(path, token, token_length);
    path[token_length] = '\0';
    if (realpath(path, resolved) == NULL) {
        (void)fprintf(
            stderr,
            "[cvite] could not resolve dependency '%s': %s\n",
            path,
            strerror(errno));
        return -1;
    }
    return watch_set_add_path(set, resolved);
}

static int parse_dependency_file(
    const cvite_run_state *state,
    cvite_build_kind kind,
    cvite_watch_set *set)
{
    char dependency_path[PATH_MAX];
    char *content = NULL;
    char token[PATH_MAX];
    size_t content_size = 0U;
    size_t offset;
    size_t token_length = 0U;
    size_t index;
    int result = -1;

    if (cvite_dependency_path(state, kind, dependency_path) != 0 ||
        read_file(dependency_path, &content, &content_size) != 0) {
        return -1;
    }
    if (watch_set_add_path(set, state->source_path) != 0) {
        goto cleanup;
    }

    offset = dependency_list_offset(content, content_size);
    if (offset == SIZE_MAX) {
        (void)fprintf(stderr, "[cvite] malformed Clang dependency file\n");
        goto cleanup;
    }

    for (index = offset; index <= content_size; ++index) {
        const char character = index < content_size ? content[index] : '\0';

        if (character == '\\' && index + 1U < content_size &&
            content[index + 1U] == '\n') {
            ++index;
            continue;
        }
        if (character == '\\' && index + 2U < content_size &&
            content[index + 1U] == '\r' && content[index + 2U] == '\n') {
            index += 2U;
            continue;
        }
        if (character == '\\' && index + 1U < content_size) {
            if (token_length + 1U >= sizeof(token)) {
                (void)fprintf(stderr, "[cvite] dependency path is too long\n");
                goto cleanup;
            }
            token[token_length] = content[index + 1U];
            ++token_length;
            ++index;
            continue;
        }
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n' || character == '\0') {
            if (add_dependency_token(set, token, token_length) != 0) {
                goto cleanup;
            }
            token_length = 0U;
            continue;
        }
        if (token_length + 1U >= sizeof(token)) {
            (void)fprintf(stderr, "[cvite] dependency path is too long\n");
            goto cleanup;
        }
        token[token_length] = character;
        ++token_length;
    }

    result = 0;

cleanup:
    free(content);
    return result;
}

static bool watch_sets_equal(
    const cvite_run_state *state,
    const cvite_watch_set *candidate)
{
    size_t directory_index;

    if (state->watch_directory_count != candidate->count) {
        return false;
    }
    for (directory_index = 0U;
         directory_index < candidate->count;
         ++directory_index) {
        const cvite_watch_directory *candidate_directory =
            &candidate->directories[directory_index];
        const cvite_watch_directory *active_directory =
            find_const_directory_by_path(
                state->watch_directories,
                state->watch_directory_count,
                candidate_directory->path);
        size_t file_index;

        if (active_directory == NULL ||
            active_directory->file_count != candidate_directory->file_count) {
            return false;
        }
        for (file_index = 0U;
             file_index < candidate_directory->file_count;
             ++file_index) {
            if (!directory_contains_file(
                    active_directory,
                    candidate_directory->file_names[file_index])) {
                return false;
            }
        }
    }
    return true;
}

static bool active_set_has_handle(const cvite_run_state *state, int handle)
{
    return find_directory_by_handle(state, handle) != NULL;
}

static bool candidate_set_has_handle(const cvite_watch_set *set, int handle)
{
    size_t index;

    for (index = 0U; index < set->count; ++index) {
        if (set->directories[index].handle == handle) {
            return true;
        }
    }
    return false;
}

static size_t watch_set_file_count(const cvite_watch_set *set)
{
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < set->count; ++index) {
        count += set->directories[index].file_count;
    }
    return count;
}

static int install_watch_set(
    cvite_run_state *state,
    cvite_watch_set *candidate)
{
    size_t index;

    if (watch_sets_equal(state, candidate)) {
        free_watch_set(candidate);
        return 0;
    }

    for (index = 0U; index < candidate->count; ++index) {
        cvite_watch_directory *directory = &candidate->directories[index];

        directory->handle = inotify_add_watch(
            state->watch_descriptor,
            directory->path,
            CVITE_WATCH_MASK);
        if (directory->handle < 0) {
            size_t rollback_index;

            (void)fprintf(
                stderr,
                "[cvite] could not watch dependency directory %s: %s\n",
                directory->path,
                strerror(errno));
            for (rollback_index = 0U;
                 rollback_index < index;
                 ++rollback_index) {
                const int handle = candidate->directories[rollback_index].handle;
                if (handle >= 0 && !active_set_has_handle(state, handle)) {
                    (void)inotify_rm_watch(state->watch_descriptor, handle);
                }
            }
            free_watch_set(candidate);
            return -1;
        }
    }

    for (index = 0U; index < state->watch_directory_count; ++index) {
        const int handle = state->watch_directories[index].handle;
        if (handle >= 0 && !candidate_set_has_handle(candidate, handle)) {
            (void)inotify_rm_watch(state->watch_descriptor, handle);
        }
    }

    free_watch_directories(
        state->watch_directories,
        state->watch_directory_count);
    state->watch_directories = candidate->directories;
    state->watch_directory_count = candidate->count;
    candidate->directories = NULL;
    candidate->count = 0U;
    candidate->capacity = 0U;

    {
        const cvite_watch_set active = {
            state->watch_directories,
            state->watch_directory_count,
            state->watch_directory_count,
        };
        const size_t file_count = watch_set_file_count(&active);

        (void)fprintf(
            stderr,
            "[cvite] watching %zu file%s across %zu director%s\n",
            file_count,
            file_count == 1U ? "" : "s",
            state->watch_directory_count,
            state->watch_directory_count == 1U ? "y" : "ies");
    }
    return 0;
}

int cvite_refresh_source_watcher(
    cvite_run_state *state,
    cvite_build_kind kind)
{
    cvite_watch_set candidate = {0};

    if (state == NULL || state->watch_descriptor < 0) {
        return -1;
    }
    if (parse_dependency_file(state, kind, &candidate) != 0) {
        free_watch_set(&candidate);
        return -1;
    }
    return install_watch_set(state, &candidate);
}
