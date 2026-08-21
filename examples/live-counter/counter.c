#include "counter.h"

static int counter;

void counter_step(void)
{
    counter += 1;
}

int counter_value(void)
{
    return counter;
}
