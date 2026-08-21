#include "run_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CVITE_MAX_DEPFILE_SIZE (8U * 1024U * 1024U)

static int read_file(const char *path, char **content, size_t *content_size)
{
    FILE *file;
    long length;
    size_t size;
    char *buffer;

    if (path == NULL || content == NULL || content_size == NULL) {
        return -1;
    }
    *content = NULL;
    *content_size = 0U;
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return -1;
    }
    length = ftell(file);
    if (length < 0L || (unsigned long)length > CVITE_MAX_DEPFILE_SIZE) {
        (void)fclose(file);
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

static int visit_token(
    const char *token,
    size_t token_length,
    cvite_dependency_visitor visitor,
    void *context)
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
    return visitor(resolved, context);
}

int cvite_visit_dependency_file(
    const char *dependency_path,
    cvite_dependency_visitor visitor,
    void *context)
{
    char *content = NULL;
    char token[PATH_MAX];
    size_t content_size = 0U;
    size_t offset;
    size_t token_length = 0U;
    size_t index;
    int result = -1;

    if (dependency_path == NULL || visitor == NULL ||
        read_file(dependency_path, &content, &content_size) != 0) {
        return -1;
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
            token[token_length++] = content[++index];
            continue;
        }
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n' || character == '\0') {
            if (visit_token(token, token_length, visitor, context) != 0) {
                goto cleanup;
            }
            token_length = 0U;
            continue;
        }
        if (token_length + 1U >= sizeof(token)) {
            (void)fprintf(stderr, "[cvite] dependency path is too long\n");
            goto cleanup;
        }
        token[token_length++] = character;
    }

    result = 0;

cleanup:
    free(content);
    return result;
}
