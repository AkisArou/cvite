#define _POSIX_C_SOURCE 200112L

#include "cvite/managed_memory.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
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
    void *external_slot;
    cvite_managed_object owner;
    size_t owner_offset;
    cvite_managed_object target;
    size_t target_offset;
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

typedef struct cvite_managed_pointer_write {
    void *slot;
    void *value;
} cvite_managed_pointer_write;

static int managed_id_equal(cvite_id left, cvite_id right)
{
    return left.high == right.high && left.low == right.low;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static cvite_managed_status managed_fail(
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
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
        (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
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

static int valid_alignment(size_t alignment)
{
    return alignment != 0U && (alignment & (alignment - 1U)) == 0U;
}

static size_t normalized_alignment(size_t alignment)
{
    return alignment < sizeof(void *) ? sizeof(void *) : alignment;
}

static int valid_requested_alignment(size_t alignment)
{
    return alignment != 0U && valid_alignment(normalized_alignment(alignment));
}

static void *aligned_allocate(size_t size, size_t alignment)
{
    void *address = NULL;
    const size_t normalized = normalized_alignment(alignment);
    if (size == 0U || !valid_alignment(normalized) ||
        posix_memalign(&address, normalized, size) != 0) {
        return NULL;
    }
    memset(address, 0, size);
    return address;
}

static int grow_objects(cvite_managed_domain *domain)
{
    if (domain->object_count < domain->object_capacity) {
        return 1;
    }
    const size_t capacity =
        domain->object_capacity == 0U ? 16U : domain->object_capacity * 2U;
    if (capacity < domain->object_capacity ||
        capacity > ((size_t)-1) / sizeof(*domain->objects)) {
        return 0;
    }
    void *storage = realloc(domain->objects, capacity * sizeof(*domain->objects));
    if (storage == NULL) {
        return 0;
    }
    domain->objects = (cvite_managed_record *)storage;
    domain->object_capacity = capacity;
    return 1;
}

static int grow_pointers(cvite_managed_domain *domain)
{
    if (domain->pointer_count < domain->pointer_capacity) {
        return 1;
    }
    const size_t capacity =
        domain->pointer_capacity == 0U ? 32U : domain->pointer_capacity * 2U;
    if (capacity < domain->pointer_capacity ||
        capacity > ((size_t)-1) / sizeof(*domain->pointers)) {
        return 0;
    }
    void *storage = realloc(domain->pointers, capacity * sizeof(*domain->pointers));
    if (storage == NULL) {
        return 0;
    }
    domain->pointers = (cvite_managed_pointer_record *)storage;
    domain->pointer_capacity = capacity;
    return 1;
}

static size_t find_object(
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

static int pointer_fits(size_t offset, size_t size)
{
    return offset <= size && sizeof(void *) <= size - offset;
}

static int locate_unlocked(
    const cvite_managed_domain *domain,
    const void *address,
    int allow_one_past,
    size_t *object_index,
    size_t *offset)
{
    if (address == NULL) {
        return 0;
    }
    const uintptr_t raw = (uintptr_t)address;
    size_t index = 0U;

    for (index = 0U; index < domain->object_count; ++index) {
        const cvite_managed_record *record = &domain->objects[index];
        const uintptr_t base = (uintptr_t)record->address;
        if (raw >= base && raw - base < record->size) {
            if (object_index != NULL) {
                *object_index = index;
            }
            if (offset != NULL) {
                *offset = (size_t)(raw - base);
            }
            return 1;
        }
    }

    if (!allow_one_past) {
        return 0;
    }
    for (index = 0U; index < domain->object_count; ++index) {
        const cvite_managed_record *record = &domain->objects[index];
        const uintptr_t base = (uintptr_t)record->address;
        if (raw >= base && raw - base == record->size) {
            if (object_index != NULL) {
                *object_index = index;
            }
            if (offset != NULL) {
                *offset = record->size;
            }
            return 1;
        }
    }
    return 0;
}

static void *resolve_slot_unlocked(
    const cvite_managed_domain *domain,
    const cvite_managed_pointer_record *pointer)
{
    if (pointer->owner == CVITE_MANAGED_OBJECT_INVALID) {
        return pointer->external_slot;
    }
    const size_t owner_index = find_object(domain, pointer->owner);
    if (owner_index == (size_t)-1 ||
        !pointer_fits(pointer->owner_offset, domain->objects[owner_index].size)) {
        return NULL;
    }
    return (void *)((unsigned char *)domain->objects[owner_index].address +
                    pointer->owner_offset);
}

static size_t find_pointer(const cvite_managed_domain *domain, void *slot)
{
    size_t index = 0U;
    for (index = 0U; index < domain->pointer_count; ++index) {
        if (resolve_slot_unlocked(domain, &domain->pointers[index]) == slot) {
            return index;
        }
    }
    return (size_t)-1;
}

static void *staged_address(
    const cvite_managed_domain *domain,
    const cvite_managed_staged_record *staged,
    size_t staged_count,
    size_t object_index)
{
    size_t index = 0U;
    for (index = 0U; index < staged_count; ++index) {
        if (staged[index].object_index == object_index) {
            return staged[index].new_address;
        }
    }
    return domain->objects[object_index].address;
}

cvite_managed_status cvite_managed_domain_create(
    cvite_managed_domain **domain,
    cvite_managed_error *error)
{
    if (domain == NULL) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "managed domain output must not be null");
    }
    *domain = NULL;
    cvite_managed_domain *created =
        (cvite_managed_domain *)calloc(1U, sizeof(*created));
    if (created == NULL) {
        return managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot allocate managed domain");
    }
    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        free(created);
        return managed_fail(
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
        void *slot = resolve_slot_unlocked(domain, &domain->pointers[index]);
        if (slot != NULL) {
            void *value = NULL;
            memcpy(slot, &value, sizeof(value));
        }
    }
    for (index = 0U; index < domain->object_count; ++index) {
        free(domain->objects[index].address);
    }
    free(domain->pointers);
    free(domain->objects);
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
        !valid_requested_alignment(alignment)) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid managed allocation request");
    }
    *object = CVITE_MANAGED_OBJECT_INVALID;
    *address = NULL;
    (void)pthread_mutex_lock(&domain->mutex);
    if (!grow_objects(domain)) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot grow managed object registry");
    }
    void *allocated = aligned_allocate(size, alignment);
    if (allocated == NULL) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot allocate %zu managed bytes",
            size);
    }
    if (domain->next_object == CVITE_MANAGED_OBJECT_INVALID) {
        free(allocated);
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
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
        normalized_alignment(alignment),
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
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid managed free request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t object_index = find_object(domain, object);
    if (object_index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            object,
            "managed object was not found");
    }

    size_t pointer_index = 0U;
    while (pointer_index < domain->pointer_count) {
        cvite_managed_pointer_record *pointer = &domain->pointers[pointer_index];
        const int owner_removed = pointer->owner == object;
        const int target_removed = pointer->target == object;
        if (!owner_removed && !target_removed) {
            ++pointer_index;
            continue;
        }
        if (target_removed && !owner_removed) {
            void *slot = resolve_slot_unlocked(domain, pointer);
            if (slot != NULL) {
                void *value = NULL;
                memcpy(slot, &value, sizeof(value));
            }
        }
        domain->pointers[pointer_index] =
            domain->pointers[domain->pointer_count - 1U];
        --domain->pointer_count;
    }

    free(domain->objects[object_index].address);
    domain->objects[object_index] = domain->objects[domain->object_count - 1U];
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
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid managed address request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t index = find_object(domain, object);
    if (index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
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

cvite_managed_status cvite_managed_locate(
    cvite_managed_domain *domain,
    const void *address,
    cvite_managed_object *object,
    size_t *offset,
    cvite_managed_error *error)
{
    if (domain == NULL || address == NULL || object == NULL) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid managed locate request");
    }
    *object = CVITE_MANAGED_OBJECT_INVALID;
    if (offset != NULL) {
        *offset = 0U;
    }
    (void)pthread_mutex_lock(&domain->mutex);
    size_t index = 0U;
    size_t located_offset = 0U;
    if (!locate_unlocked(domain, address, 1, &index, &located_offset)) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            CVITE_MANAGED_OBJECT_INVALID,
            "address is not part of a managed object");
    }
    *object = domain->objects[index].id;
    if (offset != NULL) {
        *offset = located_offset;
    }
    (void)pthread_mutex_unlock(&domain->mutex);
    cvite_managed_error_clear(error);
    return CVITE_MANAGED_OK;
}

static cvite_managed_status track_pointer_unlocked(
    cvite_managed_domain *domain,
    void *external_slot,
    cvite_managed_object owner,
    size_t owner_offset,
    cvite_managed_object target,
    size_t target_offset,
    cvite_managed_error *error)
{
    const size_t target_index = find_object(domain, target);
    if (target_index == (size_t)-1) {
        return managed_fail(
            error,
            CVITE_MANAGED_NOT_FOUND,
            target,
            "tracked pointer target was not found");
    }
    if (target_offset > domain->objects[target_index].size) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            target,
            "tracked pointer offset %zu exceeds target size %zu",
            target_offset,
            domain->objects[target_index].size);
    }

    void *slot = external_slot;
    if (owner != CVITE_MANAGED_OBJECT_INVALID) {
        const size_t owner_index = find_object(domain, owner);
        if (owner_index == (size_t)-1) {
            return managed_fail(
                error,
                CVITE_MANAGED_NOT_FOUND,
                owner,
                "tracked pointer owner was not found");
        }
        if (!pointer_fits(owner_offset, domain->objects[owner_index].size)) {
            return managed_fail(
                error,
                CVITE_MANAGED_INVALID_ARGUMENT,
                owner,
                "pointer slot offset %zu does not fit owner size %zu",
                owner_offset,
                domain->objects[owner_index].size);
        }
        slot = (void *)((unsigned char *)domain->objects[owner_index].address +
                        owner_offset);
        external_slot = NULL;
    } else if (slot == NULL) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            target,
            "external tracked pointer slot must not be null");
    }

    size_t pointer_index = find_pointer(domain, slot);
    if (pointer_index == (size_t)-1) {
        if (!grow_pointers(domain)) {
            return managed_fail(
                error,
                CVITE_MANAGED_OUT_OF_MEMORY,
                target,
                "cannot grow tracked pointer registry");
        }
        pointer_index = domain->pointer_count++;
    }
    domain->pointers[pointer_index] = (cvite_managed_pointer_record){
        external_slot,
        owner,
        owner_offset,
        target,
        target_offset,
    };
    {
        void *value =
            (void *)((unsigned char *)domain->objects[target_index].address +
                     target_offset);
        memcpy(slot, &value, sizeof(value));
    }
    return CVITE_MANAGED_OK;
}

cvite_managed_status cvite_managed_track_pointer(
    cvite_managed_domain *domain,
    void *slot,
    cvite_managed_object target,
    size_t target_offset,
    cvite_managed_error *error)
{
    if (domain == NULL || slot == NULL ||
        target == CVITE_MANAGED_OBJECT_INVALID) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            target,
            "invalid tracked pointer request");
    }

    (void)pthread_mutex_lock(&domain->mutex);
    cvite_managed_object owner = CVITE_MANAGED_OBJECT_INVALID;
    size_t owner_offset = 0U;
    size_t owner_index = 0U;
    const int owner_located = locate_unlocked(
        domain,
        (const void *)slot,
        0,
        &owner_index,
        &owner_offset);
    if (owner_located &&
        !pointer_fits(owner_offset, domain->objects[owner_index].size)) {
        const cvite_managed_object invalid_owner =
            domain->objects[owner_index].id;
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            invalid_owner,
            "pointer slot offset %zu does not fit owner size %zu",
            owner_offset,
            domain->objects[owner_index].size);
    }
    if (owner_located) {
        owner = domain->objects[owner_index].id;
    }

    const cvite_managed_status status = track_pointer_unlocked(
        domain,
        owner == CVITE_MANAGED_OBJECT_INVALID ? slot : NULL,
        owner,
        owner_offset,
        target,
        target_offset,
        error);
    (void)pthread_mutex_unlock(&domain->mutex);
    if (status == CVITE_MANAGED_OK) {
        cvite_managed_error_clear(error);
    }
    return status;
}

cvite_managed_status cvite_managed_track_pointer_in_object(
    cvite_managed_domain *domain,
    cvite_managed_object owner,
    size_t owner_offset,
    cvite_managed_object target,
    size_t target_offset,
    cvite_managed_error *error)
{
    if (domain == NULL || owner == CVITE_MANAGED_OBJECT_INVALID ||
        target == CVITE_MANAGED_OBJECT_INVALID) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            target,
            "invalid owner-relative tracked pointer request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const cvite_managed_status status = track_pointer_unlocked(
        domain,
        NULL,
        owner,
        owner_offset,
        target,
        target_offset,
        error);
    (void)pthread_mutex_unlock(&domain->mutex);
    if (status == CVITE_MANAGED_OK) {
        cvite_managed_error_clear(error);
    }
    return status;
}

cvite_managed_status cvite_managed_untrack_pointer(
    cvite_managed_domain *domain,
    void *slot,
    cvite_managed_error *error)
{
    if (domain == NULL || slot == NULL) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid untrack request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t index = find_pointer(domain, slot);
    if (index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
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
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            object,
            "invalid managed escape request");
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t index = find_object(domain, object);
    if (index == (size_t)-1) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
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
        !valid_requested_alignment(new_alignment)) {
        return managed_fail(
            error,
            CVITE_MANAGED_INVALID_ARGUMENT,
            CVITE_MANAGED_OBJECT_INVALID,
            "invalid managed migration request");
    }

    (void)pthread_mutex_lock(&domain->mutex);
    size_t count = 0U;
    size_t index = 0U;
    for (index = 0U; index < domain->object_count; ++index) {
        if (!managed_id_equal(domain->objects[index].type, type)) {
            continue;
        }
        if (domain->objects[index].escaped) {
            const cvite_managed_object escaped = domain->objects[index].id;
            (void)pthread_mutex_unlock(&domain->mutex);
            return managed_fail(
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
    if (count > ((size_t)-1) / sizeof(cvite_managed_staged_record) ||
        domain->pointer_count >
            ((size_t)-1) / sizeof(cvite_managed_pointer_write)) {
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "managed migration staging size overflowed");
    }

    for (index = 0U; index < domain->pointer_count; ++index) {
        const cvite_managed_pointer_record *pointer = &domain->pointers[index];
        const size_t target_index = find_object(domain, pointer->target);
        if (target_index == (size_t)-1) {
            (void)pthread_mutex_unlock(&domain->mutex);
            return managed_fail(
                error,
                CVITE_MANAGED_INVALID_STATE,
                pointer->target,
                "tracked pointer target disappeared");
        }
        const size_t target_size =
            managed_id_equal(domain->objects[target_index].type, type)
                ? new_size
                : domain->objects[target_index].size;
        if (pointer->target_offset > target_size) {
            (void)pthread_mutex_unlock(&domain->mutex);
            return managed_fail(
                error,
                CVITE_MANAGED_INVALID_STATE,
                pointer->target,
                "tracked target offset %zu exceeds new size %zu",
                pointer->target_offset,
                target_size);
        }

        if (pointer->owner != CVITE_MANAGED_OBJECT_INVALID) {
            const size_t owner_index = find_object(domain, pointer->owner);
            if (owner_index == (size_t)-1) {
                (void)pthread_mutex_unlock(&domain->mutex);
                return managed_fail(
                    error,
                    CVITE_MANAGED_INVALID_STATE,
                    pointer->owner,
                    "tracked pointer owner disappeared");
            }
            const size_t owner_size =
                managed_id_equal(domain->objects[owner_index].type, type)
                    ? new_size
                    : domain->objects[owner_index].size;
            if (!pointer_fits(pointer->owner_offset, owner_size)) {
                (void)pthread_mutex_unlock(&domain->mutex);
                return managed_fail(
                    error,
                    CVITE_MANAGED_INVALID_STATE,
                    pointer->owner,
                    "pointer slot offset %zu does not fit new owner size %zu",
                    pointer->owner_offset,
                    owner_size);
            }
        } else if (pointer->external_slot == NULL) {
            (void)pthread_mutex_unlock(&domain->mutex);
            return managed_fail(
                error,
                CVITE_MANAGED_INVALID_STATE,
                pointer->target,
                "external pointer slot disappeared");
        }
    }

    cvite_managed_staged_record *staged =
        (cvite_managed_staged_record *)calloc(count, sizeof(*staged));
    cvite_managed_pointer_write *writes = NULL;
    if (domain->pointer_count > 0U) {
        writes = (cvite_managed_pointer_write *)calloc(
            domain->pointer_count,
            sizeof(*writes));
    }
    if (staged == NULL || (domain->pointer_count > 0U && writes == NULL)) {
        free(writes);
        free(staged);
        (void)pthread_mutex_unlock(&domain->mutex);
        return managed_fail(
            error,
            CVITE_MANAGED_OUT_OF_MEMORY,
            CVITE_MANAGED_OBJECT_INVALID,
            "cannot allocate managed migration staging records");
    }

    size_t staged_count = 0U;
    for (index = 0U; index < domain->object_count; ++index) {
        cvite_managed_record *record = &domain->objects[index];
        if (!managed_id_equal(record->type, type)) {
            continue;
        }
        void *replacement = aligned_allocate(new_size, new_alignment);
        if (replacement == NULL) {
            const cvite_managed_status status = managed_fail(
                error,
                CVITE_MANAGED_OUT_OF_MEMORY,
                record->id,
                "cannot allocate replacement for managed object %llu",
                (unsigned long long)record->id);
            while (staged_count > 0U) {
                free(staged[--staged_count].new_address);
            }
            free(writes);
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
            free(writes);
            free(staged);
            (void)pthread_mutex_unlock(&domain->mutex);
            return managed_fail(
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

    /* Resolve every future pointer write before publication begins. */
    for (index = 0U; index < domain->pointer_count; ++index) {
        const cvite_managed_pointer_record *pointer = &domain->pointers[index];
        const size_t target_index = find_object(domain, pointer->target);
        void *target_address =
            staged_address(domain, staged, staged_count, target_index);
        void *slot = pointer->external_slot;
        if (pointer->owner != CVITE_MANAGED_OBJECT_INVALID) {
            const size_t owner_index = find_object(domain, pointer->owner);
            void *owner_address =
                staged_address(domain, staged, staged_count, owner_index);
            slot = (void *)((unsigned char *)owner_address +
                            pointer->owner_offset);
        }
        writes[index].slot = slot;
        writes[index].value =
            (void *)((unsigned char *)target_address + pointer->target_offset);
    }

    /* No operation below this point can fail. */
    for (index = 0U; index < staged_count; ++index) {
        cvite_managed_record *record =
            &domain->objects[staged[index].object_index];
        record->address = staged[index].new_address;
        record->size = new_size;
        record->alignment = normalized_alignment(new_alignment);
    }
    for (index = 0U; index < domain->pointer_count; ++index) {
        memcpy(
            writes[index].slot,
            &writes[index].value,
            sizeof(writes[index].value));
    }
    for (index = 0U; index < staged_count; ++index) {
        free(staged[index].old_address);
    }

    if (migrated_count != NULL) {
        *migrated_count = staged_count;
    }
    free(writes);
    free(staged);
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
        !valid_requested_alignment(plan->alignment) ||
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

size_t cvite_managed_pointer_count(cvite_managed_domain *domain)
{
    if (domain == NULL) {
        return 0U;
    }
    (void)pthread_mutex_lock(&domain->mutex);
    const size_t count = domain->pointer_count;
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
