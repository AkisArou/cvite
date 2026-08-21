#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <time.h>

int cvite_auto_native_bonus(void);

static int total = 0;

static int step(void)
{
    total += cvite_auto_native_bonus();
    return total;
}

int main(void)
{
    const struct timespec pause = {0, 20000000L};
    for (int iteration = 0; iteration < 450; ++iteration) {
        (void)printf("value=%d\n", step());
        (void)fflush(stdout);
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}
