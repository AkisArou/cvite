#define _POSIX_C_SOURCE 200809L

#include "run_step.h"

#include <stdio.h>
#include <time.h>

static int total = 0;

static int step(void)
{
    total += CVITE_STEP;
    return total;
}

int main(int argc, char **argv)
{
    struct timespec pause = {0, 20000000L};
    int iteration;

    (void)argc;
    (void)argv;
    for (iteration = 0; iteration < 600; ++iteration) {
        (void)printf("value=%d\n", step());
        (void)fflush(stdout);
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}
