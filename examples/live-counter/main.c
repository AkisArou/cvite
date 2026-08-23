#define _POSIX_C_SOURCE 200809L

#include "counter.h"

#include <stdio.h>
#include <time.h>

static void sleep_for_milliseconds(long milliseconds)
{
    const struct timespec duration = {
        milliseconds / 1000L,
        (milliseconds % 1000L) * 1000000L,
    };
    (void)nanosleep(&duration, NULL);
}

int main(void)
{
    for (;;) {
        counter_step();
        printf("counter = %d\n", counter_value());
        fflush(stdout);
        sleep_for_milliseconds(250L);
    }
}
