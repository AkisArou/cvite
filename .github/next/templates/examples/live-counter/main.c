#define _POSIX_C_SOURCE 200809L

#include "counter.h"

#include <stdio.h>
#include <time.h>

int main(void)
{
    int value = 0;
    struct timespec delay = {0, 200000000L};
    for (;;) {
        value = counter_step(value);
        printf("counter=%d\n", value);
        fflush(stdout);
        (void)nanosleep(&delay, NULL);
    }
}
