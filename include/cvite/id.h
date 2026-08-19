#ifndef CVITE_ID_H
#define CVITE_ID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cvite_id {
    uint64_t high;
    uint64_t low;
} cvite_id;

#define CVITE_ID_INITIALIZER(HIGH, LOW) {(uint64_t)(HIGH), (uint64_t)(LOW)}
#define CVITE_ID(HIGH, LOW) ((cvite_id)CVITE_ID_INITIALIZER(HIGH, LOW))
#define CVITE_ID_ZERO CVITE_ID(0U, 0U)

static inline bool cvite_id_equal(cvite_id left, cvite_id right)
{
    return left.high == right.high && left.low == right.low;
}

static inline bool cvite_id_is_zero(cvite_id value)
{
    return value.high == 0U && value.low == 0U;
}

#ifdef __cplusplus
}
#endif

#endif
