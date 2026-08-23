#include "cvite/managed_host.h"

#include <stdatomic.h>
#include <stdio.h>

#include "cvite/host.h"

static atomic_flag cvite_managed_host_lock_flag = ATOMIC_FLAG_INIT;
static cvite_managed_heap *cvite_managed_host_heap = NULL;

static void cvite_managed_host_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
        &cvite_managed_host_lock_flag, memory_order_acquire)) {
    }
}

static void cvite_managed_host_unlock(void)
{
    atomic_flag_clear_explicit(
        &cvite_managed_host_lock_flag, memory_order_release);
}

static cvite_managed_status cvite_managed_host_require_heap(
    cvite_managed_error *error)
{
    if (cvite_managed_host_heap != NULL) {
        return CVITE_MANAGED_STATUS_OK;
    }
    return cvite_managed_heap_create(&cvite_managed_host_heap, error);
}

cvite_managed_status cvite_host_managed_register_object(
    const cvite_managed_object_definition *definition,
    void **address,
    cvite_managed_error *error)
{
    cvite_managed_status status = CVITE_MANAGED_STATUS_OK;

    cvite_managed_host_lock();
    status = cvite_managed_host_require_heap(error);
    if (status == CVITE_MANAGED_STATUS_OK) {
        status = cvite_managed_heap_register_object(
            cvite_managed_host_heap, definition, address, error);
    }
    cvite_managed_host_unlock();
    return status;
}

cvite_managed_status cvite_host_managed_find_object(
    cvite_managed_id id,
    cvite_managed_object_view *view,
    cvite_managed_error *error)
{
    cvite_managed_status status = CVITE_MANAGED_STATUS_OK;

    cvite_managed_host_lock();
    status = cvite_managed_host_require_heap(error);
    if (status == CVITE_MANAGED_STATUS_OK) {
        status = cvite_managed_heap_find_object(
            cvite_managed_host_heap, id, view, error);
    }
    cvite_managed_host_unlock();
    return status;
}

cvite_managed_status cvite_host_managed_track_pointer(
    const cvite_managed_pointer_definition *definition,
    cvite_managed_error *error)
{
    cvite_managed_status status = CVITE_MANAGED_STATUS_OK;

    cvite_managed_host_lock();
    status = cvite_managed_host_require_heap(error);
    if (status == CVITE_MANAGED_STATUS_OK) {
        status = cvite_managed_heap_track_pointer(
            cvite_managed_host_heap, definition, error);
    }
    cvite_managed_host_unlock();
    return status;
}

cvite_managed_status cvite_host_managed_migrate(
    const cvite_managed_migration *migration,
    cvite_managed_error *error)
{
    cvite_managed_status status = CVITE_MANAGED_STATUS_OK;

    if (!cvite_host_try_begin_quiescence()) {
        cvite_managed_error_clear(error);
        if (error != NULL) {
            error->status = CVITE_MANAGED_STATUS_BUSY;
            error->object_id = CVITE_MANAGED_NULL_ID;
            (void)snprintf(
                error->message,
                sizeof(error->message),
                "%s",
                "managed migration deferred until native calls quiesce");
        }
        return CVITE_MANAGED_STATUS_BUSY;
    }

    cvite_managed_host_lock();
    status = cvite_managed_host_require_heap(error);
    if (status == CVITE_MANAGED_STATUS_OK) {
        status = cvite_managed_heap_migrate(
            cvite_managed_host_heap, migration, error);
    }
    cvite_managed_host_unlock();
    cvite_host_end_quiescence();
    return status;
}

size_t cvite_host_managed_object_count(void)
{
    size_t count = (size_t)0;

    cvite_managed_host_lock();
    if (cvite_managed_host_heap != NULL) {
        count = cvite_managed_heap_object_count(cvite_managed_host_heap);
    }
    cvite_managed_host_unlock();
    return count;
}

size_t cvite_host_managed_pointer_count(void)
{
    size_t count = (size_t)0;

    cvite_managed_host_lock();
    if (cvite_managed_host_heap != NULL) {
        count = cvite_managed_heap_pointer_count(cvite_managed_host_heap);
    }
    cvite_managed_host_unlock();
    return count;
}

void cvite_host_managed_shutdown(void)
{
    cvite_managed_host_lock();
    cvite_managed_heap_destroy(cvite_managed_host_heap);
    cvite_managed_host_heap = NULL;
    cvite_managed_host_unlock();
}
