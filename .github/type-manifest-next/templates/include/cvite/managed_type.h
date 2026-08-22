#ifndef CVITE_MANAGED_TYPE_H
#define CVITE_MANAGED_TYPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CVITE_MANAGED_TYPE_MANIFEST_SCHEMA UINT64_C(1)
#define CVITE_MANAGED_TYPE_MANIFEST_SYMBOL "__cvite_managed_type_manifest"

#define CVITE_MANAGED_FIELD_BIT_FIELD UINT64_C(1)
#define CVITE_MANAGED_FIELD_FLEXIBLE_ARRAY UINT64_C(2)
#define CVITE_MANAGED_FIELD_NON_BYTE_ADDRESSABLE UINT64_C(4)

typedef struct cvite_managed_field_record {
    uint64_t id_high;
    uint64_t id_low;
    uint64_t type_high;
    uint64_t type_low;
    uint64_t offset;
    uint64_t size;
    uint64_t flags;
    const char *debug_name;
} cvite_managed_field_record;

typedef struct cvite_managed_type_record {
    uint64_t id_high;
    uint64_t id_low;
    uint64_t layout_high;
    uint64_t layout_low;
    uint64_t size;
    uint64_t alignment;
    uint64_t field_count;
    const cvite_managed_field_record *fields;
    const char *debug_name;
} cvite_managed_type_record;

typedef struct cvite_managed_type_manifest {
    uint64_t schema;
    uint64_t type_count;
    const cvite_managed_type_record *types;
} cvite_managed_type_manifest;

#ifdef __cplusplus
}
#endif

#endif
