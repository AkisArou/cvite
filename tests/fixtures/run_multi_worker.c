#include "run_multi_shared.h"

static int total = 0;

static int same_name(void)
{
    return 0;
}

int worker_step(void)
{
    total += CVITE_MULTI_STEP + same_name();
    return total;
}
