#ifndef CVITE_MANAGED_HOST_H
#define CVITE_MANAGED_HOST_H

#include <stddef.h>

#include "cvite/managed_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal process-wide bridge between compiler-generated allocation/pointer
 * instrumentation and the managed-memory transaction core. Application source
 * never includes or calls this API.
 */

cvite_managed_status cvite_host_managed_register_object(
    const cvite_managed_object_definition *definition,
    void **address,
    cvite_managed_error *error);

cvite_managed_status cvite_host_managed_find_object(
    cvite_managed_id id,
    cvite_managed_object_view *view,
    cvite_managed_error *error);

cvite_managed_status cvite_host_managed_track_pointer(
    const cvite_managed_pointer_definition *definition,
    cvite_managed_error *error);

/*
 * Attempts a stop-the-world managed migration. If a generated native call is
 * active, the function returns CVITE_MANAGED_STATUS_BUSY without changing any
 * object, pointer, or layout state; callers may retry at the next safe point.
 */
cvite_managed_status cvite_host_managed_migrate(
    const cvite_managed_migration *migration,
    cvite_managed_error *error);

size_t cvite_host_managed_object_count(void);
size_t cvite_host_managed_pointer_count(void);

/* Process/test teardown. Must not race application or compiler callbacks. */
void cvite_host_managed_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
