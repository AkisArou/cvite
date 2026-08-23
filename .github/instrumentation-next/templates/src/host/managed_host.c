#define _POSIX_C_SOURCE 200809L

#include "cvite/managed_memory.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_once_t cvite_managed_once = PTHREAD_ONCE_INIT;
static cvite_managed_domain *cvite_managed_global_domain;
static int cvite_managed_global_failed;

static void cvite_managed_global_destroy(void)
{
    cvite_managed_domain_destroy(cvite_managed_global_domain);
    cvite_managed_global_domain = NULL;
}

static void cvite_managed_global_initialize(void)
{
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    if (cvite_managed_domain_create(
            &cvite_managed_global_domain, &error) != CVITE_MANAGED_OK) {
        cvite_managed_global_failed = 1;
        (void)fprintf(
            stderr,
            "[cvite] managed allocation runtime disabled: %s\n",
            error.message);
        return;
    }
    if (atexit(cvite_managed_global_destroy) != 0) {
        cvite_managed_global_failed = 1;
        cvite_managed_domain_destroy(cvite_managed_global_domain);
        cvite_managed_global_domain = NULL;
        (void)fprintf(
            stderr,
            "[cvite] managed allocation runtime disabled: cannot register cleanup\n");
    }
}

static cvite_managed_domain *cvite_managed_domain_instance(void)
{
    (void)pthread_once(&cvite_managed_once, cvite_managed_global_initialize);
    return cvite_managed_global_failed ? NULL : cvite_managed_global_domain;
}

void *__cvite_host_managed_allocate(
    uint64_t type_high,
    uint64_t type_low,
    uint64_t size,
    uint64_t alignment)
{
    if (size == UINT64_C(0) || size > (uint64_t)SIZE_MAX ||
        alignment > (uint64_t)SIZE_MAX) {
        return NULL;
    }
    cvite_managed_domain *domain = cvite_managed_domain_instance();
    if (domain == NULL) {
        return malloc((size_t)size);
    }
    cvite_managed_object object = CVITE_MANAGED_OBJECT_INVALID;
    void *address = NULL;
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    const size_t requested_alignment = alignment == UINT64_C(0)
        ? _Alignof(max_align_t)
        : (size_t)alignment;
    if (cvite_managed_allocate(
            domain,
            (cvite_id){type_high, type_low},
            (size_t)size,
            requested_alignment,
            &object,
            &address,
            &error) != CVITE_MANAGED_OK) {
        (void)fprintf(
            stderr,
            "[cvite] managed allocation failed: %s\n",
            error.message);
        return NULL;
    }
    return address;
}

void *__cvite_host_managed_callocate(
    uint64_t type_high,
    uint64_t type_low,
    uint64_t count,
    uint64_t element_size,
    uint64_t alignment)
{
    if (element_size != UINT64_C(0) && count > UINT64_MAX / element_size) {
        return NULL;
    }
    const uint64_t total = count * element_size;
    if (total == UINT64_C(0)) {
        return calloc((size_t)count, (size_t)element_size);
    }
    return __cvite_host_managed_allocate(
        type_high, type_low, total, alignment);
}

void __cvite_host_managed_free(void *address)
{
    if (address == NULL) {
        return;
    }
    cvite_managed_domain *domain = cvite_managed_domain_instance();
    if (domain == NULL) {
        free(address);
        return;
    }
    cvite_managed_object object = CVITE_MANAGED_OBJECT_INVALID;
    size_t offset = 0U;
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    if (cvite_managed_resolve_pointer(
            domain, address, &object, &offset, &error) != CVITE_MANAGED_OK) {
        free(address);
        return;
    }
    if (offset != 0U) {
        (void)cvite_managed_mark_escaped(domain, object, &error);
        (void)fprintf(
            stderr,
            "[cvite] ignored invalid free of managed interior pointer at offset %zu\n",
            offset);
        return;
    }
    if (cvite_managed_free(domain, object, &error) != CVITE_MANAGED_OK) {
        (void)fprintf(
            stderr,
            "[cvite] managed free failed: %s\n",
            error.message);
    }
}

void __cvite_host_managed_track_pointer(void **slot, void *address)
{
    if (slot == NULL) {
        return;
    }
    cvite_managed_domain *domain = cvite_managed_domain_instance();
    if (domain == NULL) {
        return;
    }
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    if (address == NULL) {
        (void)cvite_managed_untrack_pointer(domain, slot, &error);
        return;
    }
    cvite_managed_object object = CVITE_MANAGED_OBJECT_INVALID;
    size_t offset = 0U;
    if (cvite_managed_resolve_pointer(
            domain, address, &object, &offset, &error) != CVITE_MANAGED_OK) {
        (void)cvite_managed_untrack_pointer(domain, slot, &error);
        return;
    }
    (void)cvite_managed_track_pointer(
        domain, slot, object, offset, &error);
}

void __cvite_host_managed_escape_pointer(void *address)
{
    if (address == NULL) {
        return;
    }
    cvite_managed_domain *domain = cvite_managed_domain_instance();
    if (domain == NULL) {
        return;
    }
    cvite_managed_object object = CVITE_MANAGED_OBJECT_INVALID;
    size_t offset = 0U;
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    if (cvite_managed_resolve_pointer(
            domain, address, &object, &offset, &error) == CVITE_MANAGED_OK) {
        (void)offset;
        (void)cvite_managed_mark_escaped(domain, object, &error);
    }
}
