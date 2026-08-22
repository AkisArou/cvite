#define _POSIX_C_SOURCE 200112L

#include "cvite/managed_memory.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cvite_managed_record {
    cvite_managed_object id;
    cvite_id type;
    void *address;
    size_t size;
    size_t alignment;
    int escaped;
} cvite_managed_record;

typedef struct cvite_managed_pointer_record {
    void **slot;
    cvite_managed_object object;
    size_t offset;
} cvite_managed_pointer_record;

struct cvite_managed_domain {
    pthread_mutex_t mutex;
    cvite_managed_record *objects;
    size_t object_count;
    size_t object_capacity;
    cvite_managed_pointer_record *pointers;
    size_t pointer_count;
    size_t pointer_capacity;
    cvite_managed_object next_object;
};

typedef struct cvite_managed_staged_record {
    size_t object_index;
    void *old_address;
    void *new_address;
} cvite_managed_staged_record;

static int cvite_managed_id_equal(cvite_id left, cvite_id right)
{
    return left.high == right.high && left.low == right.low;
}

static cvite_managed_status cvite_managed_fail(
    cvite_managed_error *error,
    cvite_managed_status status,
    cvite_managed_object object,
    const char *format,
    ...)
{
    if (error != NULL) {
        va_list arguments;
        error->status = status;
        error->object = object;
        va_start(arguments, format);
        (void)vsnprintf(
            error->message,
            sizeof(error->message),
            format,
            arguments);
        va_end(arguments);
    }
    return status;
}

void cvite_managed_error_clear(cvite_managed_error *error)
{
    if (error == NULL) {
        return;
    }
    error->status = CVITE_MANAGED_OK;
    error->object = CVITE_MANAGED_OBJECT_INVALID;
    error->message[0] = '\0';
}

static int cvite_managed_valid_alignment(size_t alignment)
{
    return alignment != 0U && (alignment & (alignment - 1U)) == 0U;
}

static size_t cvite_managed_normalize_alignment(size_t alignment)
{
    return alignment < sizeof(void *) ? sizeof(void *) : alignment;
}

static void *cvite_managed_aligned_allocate(size_t size, size_t alignment)
{
    void *address = NULL;
    const size_t normalized = cvite_managed_normalize_alignment(alignment);
    if (!cvite_managed_valid_alignment(normalized) ||
        posix_memalign(&address, normalized, size) != 0) {
        return NULL;
    }
    memset(address, 0, size);
    return address;
}

static int cvite_managed_grow_objects(cvite_managed_domain *domain)
{
    if (domain->object_count < domain->object_capacity) {
        return 1;
    }
    const size_t new_capacity =
        domain->object_capacity == 0U ? 16U : domain->object_capacity * 2U;
    if (new_capacity < domain->object_capacity ||
        new_capacity > ((size_t)-1) / sizeof(*domain->objects)) {
        return 0;
    }
    void *storage = realloc(
        domain->objects,
        new_capacity * sizeof(*domain->objects));
    if (storage == NULL) {
        return 0;
    }
    domain->objects = (cvite_managed_record *)storage;
    domain->object_capacity = new_capacity;
    return 1;
}

static int cvite_managed_grow_pointers(cvite_managed_domain *domain)
{
    if (domain->pointer_count < domain->pointer_capacity) {
        return 1;
    }
    const size_t new_capacity =
        domain->pointer_capacity == 0U ? 32U : domain->pointer_capacity * 2U;
    if (new_capacity < domain->pointer_capacity ||
        new_capacity > ((size_t)-1) / sizeof(*domain->pointers)) {
        return 0;
    }
    void *storage = realloc(
        domain->pointers,
        new_capacity * sizeof(*domain->pointers));
    if (storage == NULL) {
        return 0;
    }
    domain->pointers = (cvite_managed_pointer_record *)storage;
    domain->pointer_capacity = new_capacity;
    return 1;
}

static size_t cvite_managed_find_object(
    const cvite_managed_domain *domain,
    cvite_managed_object object)
{
    size_t index = 0U;
    for (index = 0U; index < domain->object_count; ++index) {
        if (domain->objects[index].id == object) {
            return index;
        }
    }
    return (size_t)-1;
}

static size_t cvite_managed_find_pointer(
    const cvite_managed_domain *domain,
    void **slot)
{
    size_t index = 0U;
    for (index = 0U; index < domain->pointer_count; ++index) {
        if (domain->pointers[index].slot == slot) {
            return index;
        }
    }
    return (size_t)-1;
}

cvite_managed_status cvite_managed_domain_create(
    cvite_managed_domain **domain,
    cvite_managed_error *error)
{
    if (domain == NULL) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "managed domain output must not be null");
    }
    *domain = NULL;
    cvite_managed_domain *created =
        (cvite_managed_domain *)calloc(1U, sizeof(*created));
    if (created == NULL) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot allocate managed domain");
    }
    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        free(created);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_STATE,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot initialize managed domain mutex");
    }
    created->next_object = UINT64_C(1);
    *domain = created;
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

void cvite_managed_domain_destroy(cvite_managed_domain *domain)
{
    if (domain == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&domain->mutex);
    size_t index = 0U;
    for (index = 0U; index < domain->pointer_count; ++index) {
        if (domain->pointers[index].slot != NULL) {
            *domain->pointers[index].slot = NULL;
        }
    }
    for (index = 0U; index < domain->object_count; ++index) {
        free(domain->objects[index].address);
    }
    free(domain->pointers);
    free(domain->objects);
    domain->pointers = NULL;
    domain->objects = NULL;
    domain->pointer_count = 0U;
    domain->object_count = 0U;
    (void)pthread_mutex_unlock(&domain->mutex);
    (void)pthread_mutex_destroy(&domain->mutex);
    free(domain);
}

cvite_managed_status cvite_managed_allocate(
    cvite_managed_domain *domain,
    cvite_id type,
    size_t size,
    size_t alignment,
    cvite_managed_object *object,
    void **address,
    cvite_managed_error *error)
{
    if (domain == NULL || object == NULL || address == NULL || size == 0U ||
        (type.high == UINT64_C(0) && type.low == UINT64_C(0)) ||
        !cvite_managed_valid_alignment(
            cvite_managed_normalize_alignment(alignment))) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid managed allocation request");
    }
    *object = CVITE_MANAGED_OBJECT_INVALID;
    *address = NULL;
    (void)pthread_mutex_lock(&domain->mutex);
    if (!cvite_managed_grow_objects(domain)) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot grow managed object registry");
    }
    void *allocated = cvite_managed_aligned_allocate(size, alignment);
    if (allocated == NULL) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot allocate %zu managed bytes",
            size);
    }
    if (domain->next_object == CVITE_MANAGED_OBJECT_INVALID) {
        free(allocated);
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_STATE,
            CVITE_MANAGED_OBJECT_INVALID,
            "managed object identifiers were exhausted");
    }
    const cvite_managed_object identifier = domain->next_object++;
    domain->objects[domain->object_count++] = (cvite_managed_record){
        identifier,
        type,
        allocated,
        size,
        cvite_managed_normalize_alignment(alignment),
        0,
    };
    *object = identifier;
    *address = allocated;
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_free(
    cvite_managed_domain *domain,
    cvite_managed_object object,
    cvite_managed_error *error)
{
    if (domain == NULL || object == CVITE_MANAGED_OBJECT_INVALID) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid managed free request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t object_index = cvite_managed_find_object(domain, object);
    if (object_index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            object,
            "managed object was not found");
    }
    size_t pointer_index = 0U;
    while (pointer_index < domain->pointer_count) {
        cvite_managed_pointer_record *pointer =
            &domain->pointers[pointer_index];
        if (pointer->object != object) {
            ++pointer_index;
            continue;
        }
        if (pointer->slot != NULL) {
            *pointer->slot = NULL;
        }
        domain->pointers[pointer_index] =
            domain->pointers[domain->pointer_count - 1U];
        --domain->pointer_count;
    }
    free(domain->objects[object_index].address);
    domain->objects[object_index] =
        domain->objects[domain->object_count - 1U];
    --domain->object_count;
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_address(
    cvite_managed_domain *domain,
    cvite_managed_object object,
    void **address,
    size_t *size,
    cvite_managed_error *error)
{
    if (domain == NULL || address == NULL ||
        object == CVITE_MANAGED_OBJECT_INVALID) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid managed address request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t index = cvite_managed_find_object(domain, object);
    if (index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            object,
            "managed object was not found");
    }
    *address = domain->objects[index].address;
    if (size != NULL) {
        *size = domain->objects[index].size;
    }
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_track_pointer(
    cvite_managed_domain *domain,
    void **slot,
    cvite_managed_object object,
    size_t offset,
    cvite_managed_error *error)
{
    if (domain == NULL || slot == NULL ||
        object == CVITE_MANAGED_OBJECT_INVALID) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid tracked pointer request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t object_index = cvite_managed_find_object(domain, object);
    if (object_index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            object,
            "tracked pointer object was not found");
    }
    if (offset > domain->objects[object_index].size) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "tracked pointer offset %zu exceeds object size %zu",
            offset,
            domain->objects[object_index].size);
    }
    size_t pointer_index = cvite_managed_find_pointer(domain, slot);
    if (pointer_index == (size_t)-1) {
        if (!cvite_managed_grow_pointers(domain)) {
            (void)pthread_mutex_unlock(&domain->mutex);
            return cvite_managed_fail(
                error,
                CVITE_MANAGED_OUT_OF_MEMORY,
                object,
                "cannot grow tracked pointer registry");
        }
        pointer_index = domain->pointer_count++;
    }
    domain->pointers[pointer_index] = (cvite_managed_pointer_record){
        slot,
        object,
        offset,
    };
    *slot = (void *)((unsigned char *)domain->objects[object_index].address +
                     offset);
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_untrack_pointer(
    cvite_managed_domain *domain,
    void **slot,
    cvite_managed_error *error)
{
    if (domain == NULL || slot == NULL) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid untrack request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t index = cvite_managed_find_pointer(domain, slot);
    if (index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            CVITE_MANAGED_OBJECT_INVALID,
            "tracked pointer slot was not found");
    }
    domain->pointers[index] = domain->pointers[domain->pointer_count - 1U];
    --domain->pointer_count;
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_mark_escaped(
    cvite_managed_domain *domain,
    cvite_managed_object object,
    cvite_managed_error *error)
{
    if (domain == NULL || object == CVITE_MANAGED_OBJECT_INVALID) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid managed escape request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t index = cvite_managed_find_object(domain, object);
    if (index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            object,
            "escaped managed object was not found");
    }
    domain->objects[index].escaped = 1;
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_migrate_type(
    cvite_managed_domain *domain,
    cvite_id type,
    size_t new_size,
    size_t new_alignment,
    cvite_managed_migrator migrator,
    void *context,
    size_t *migrated_count,
    cvite_managed_error *error)
{
    if (migrated_count != NULL) {
        *migrated_count = 0U;
    }
    if (domain == NULL || migrator == NULL || new_size == 0U ||
        !cvite_managed_valid_alignment(
            cvite_managed_normalize_alignment(new_alignment))) {
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid managed migration request");
    }

    (void)pthread_mutex_lock(&domain->mutex);
    size_t count = 0U;
    size_t index = 0U;
    for (index = 0U; index < domain->object_count; ++index) {
        if (!cvite_managed_id_equal(domain->objects[index].type, type)) {
            continue;
        }
        if (domain->objects[index].escaped) {
            const cvite_managed_object escaped = domain->objects[index].id;
            (void)pthread_mutex_unlock(&domain->mutex);
            return cvite_managed_fail(
                error,
                CVITE_MANAGED_POINTER_ESCAPED,
                escaped,
                "managed object %llu escaped into untracked native state",
                (unsigned long long)escaped);
        }
        ++count;
    }
    if (count == 0U) {
        (void)pthread_mutex_unlock(&domain->mutex);
        cvite_managed_error_clear(error);
        return CVITE_MANAGED_OK;
    }
    if (count > ((size_t)-1) / sizeof(cvite_managed_staged_record)) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "managed migration staging size overflowed");
    }

    for (index = 0U; index < domain->pointer_count; ++index) {
        const cvite_managed_pointer_record *pointer = &domain->pointers[index];
        const size_t object_index =
            cvite_managed_find_object(domain, pointer->object);
        if (object_index != (size_t)-1 &&
            cvite_managed_id_equal(domain->objects[object_index].type, type) &&
            pointer->offset > new_size) {
            const cvite_managed_object object = pointer->object;
            (void)pthread_mutex_unlock(&domain->mutex);
            return cvite_managed_fail(
                error,
                CVITE_MANAGED_INVALID_STATE,
                object,
                "tracked interior pointer offset %zu exceeds new size %zu",
                pointer->offset,
                new_size);
        }
    }

    cvite_managed_staged_record *staged =
        (cvite_managed_staged_record *)calloc(count, sizeof(*staged));
    if (staged == NULL) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return cvite_managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot allocate managed migration staging records");
    }

    size_t staged_count = 0U;
    for (index = 0U; index < domain->object_count; ++index) {
        cvite_managed_record *record = &domain->objects[index];
        if (!cvite_managed_id_equal(record->type, type)) {
            continue;
        }
        void *replacement =
            cvite_managed_aligned_allocate(new_size, new_alignment);
        if (replacement == NULL) {
            cvite_managed_status status = cvite_managed_fail(
                error,
                CVITE_MANAGED_OUT_OF_MEMORY,
                record->id,
                "cannot allocate replacement for managed object %llu",
                (unsigned long long)record->id);
            while (staged_count > 0U) {
                free(staged[--staged_count].new_address);
            }
            free(staged);
            (void)pthread_mutex_unlock(&domain->mutex);
            return status;
        }
        char message[256];
        message[0] = '\0';
        if (!migrator(
                record->id,
                record->address,
                record->size,
                replacement,
                new_size,
                context,
                message,
                sizeof(message))) {
            free(replacement);
            while (staged_count > 0U) {
                free(staged[--staged_count].new_address);
            }
            free(staged);
            (void)pthread_mutex_unlock(&domain->mutex);
            return cvite_managed_fail(
                error,
                CVITE_MANAGED_MIGRATOR_FAILED,
                record->id,
                "%s",
                message[0] == '\0' ? "managed migrator rejected object"
                                    : message);
        }
        staged[staged_count++] = (cvite_managed_staged_record){
            index,
            record->address,
            replacement,
        };
    }

    for (index = 0U; index < staged_count; ++index) {
        cvite_managed_record *record =
            &domain->objects[staged[index].object_index];
        record->address = staged[index].new_address;
        record->size = new_size;
        record->alignment =
            cvite_managed_normalize_alignment(new_alignment);
    }
    for (index = 0U; index < domain->pointer_count; ++index) {
        cvite_managed_pointer_record *pointer = &domain->pointers[index];
        const size_t object_index =
            cvite_managed_find_object(domain, pointer->object);
        if (object_index == (size_t)-1 ||
            !cvite_managed_id_equal(domain->objects[object_index].type, type)) {
            continue;
        }
        *pointer->slot =
            (void *)((unsigned char *)domain->objects[object_index].address +
                     pointer->offset);
    }
    for (index = 0U; index < staged_count; ++index) {
        free(staged[index].old_address);
    }
    free(staged);
    if (migrated_count != NULL) {
        *migrated_count = staged_count;
    }
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

int cvite_managed_apply_byte_plan(
    cvite_managed_object object,
    const void *old_data,
    size_t old_size,
    void *new_data,
    size_t new_size,
    void *context,
    char *message,
    size_t message_size)
{
    (void)object;
    const cvite_managed_byte_plan *plan =
        (const cvite_managed_byte_plan *)context;
    if (plan == NULL || old_data == NULL || new_data == NULL ||
        plan->old_size != old_size || plan->new_size != new_size ||
        (plan->copy_count > 0U && plan->copies == NULL)) {
        if (message != NULL && message_size > 0U) {
            (void)snprintf(message, message_size, "byte migration plan mismatch");
        }
        return 0;
    }
    memset(new_data, 0, new_size);
    const unsigned char *old_bytes = (const unsigned char *)old_data;
    unsigned char *new_bytes = (unsigned char *)new_data;
    size_t index = 0U;
    for (index = 0U; index < plan->copy_count; ++index) {
        const cvite_managed_copy_operation *copy = &plan->copies[index];
        if (copy->old_offset > old_size ||
            copy->size > old_size - copy->old_offset ||
            copy->new_offset > new_size ||
            copy->size > new_size - copy->new_offset) {
            if (message != NULL && message_size > 0U) {
                (void)snprintf(
                    message,
                    message_size,
                    "byte migration operation %zu exceeds its buffer",
                    index);
            }
            return 0;
        }
        memcpy(
            new_bytes + copy->new_offset,
            old_bytes + copy->old_offset,
            copy->size);
    }
    if (message != NULL && message_size > 0U) {
        message[0] = '\0';
    }
    return 1;
}

size_t cvite_managed_object_count(cvite_managed_domain *domain)
{
    if (domain == NULL) {
        return 0U;
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t count = domain->object_count;
    (void)pthread_mutex_unlock(&domain->mutex);
    return count;
}

const char *cvite_managed_status_name(cvite_managed_status status)
{
    switch (status) {
    case CVITE_MANAGED_OK:
        return "ok";
    case CVITE_MANAGED_INVALID_ARGUMENT:
        return "invalid argument";
    case CVITE_MANAGED_OUT_OF_MEMORY:
        return "out of memory";
    case CVITE_MANAGED_NOT_FOUND:
        return "not found";
    case CVITE_MANAGED_POINTER_ESCAPED:
        return "pointer escaped";
    case CVITE_MANAGED_MIGRATOR_FAILED:
        return "migrator failed";
    case CVITE_MANAGED_INVALID_STATE:
        return "invalid state";
    }
    return "unknown";
}
