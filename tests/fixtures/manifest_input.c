#include "manifest_input.h"

int external_counter = 4;
static Player player = {10.0f, 20.0f, 100};
static int pending_count;

static int helper(Player *target, int delta)
{
    static int calls = 0;
    ++calls;
    target->health -= delta;
    return calls;
}

int game_update(Player *target, int delta)
{
    external_counter += helper(target, delta);
    player.x += 1.0f;
    return target->health;
}
