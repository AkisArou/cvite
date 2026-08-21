#define _POSIX_C_SOURCE 200809L

#include "run_multi_shared.h"

#include <stdio.h>
#include <time.h>

int worker_step(void);

static int same_name(void)
{
    return CVITE_MULTI_SENTINEL;
}

int main(int argc, char **argv)
{
    struct timespec pause = {0, 20000000L};
    int iteration;

    (void)argc;
    (void)argv;
    if (same_name() != 7) {
        return 2;
    }
    for (iteration = 0; iteration < 800; ++iteration) {
        (void)printf("value=%d\n", worker_step());
        (void)fflush(stdout);
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}
