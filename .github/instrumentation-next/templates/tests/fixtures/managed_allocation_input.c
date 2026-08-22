#include <stddef.h>
#include <stdlib.h>

typedef struct Player {
    int score;
    float speed;
} Player;

static Player *global_player;

extern void foreign_retain(void *pointer);

void initialize_player(void)
{
    global_player = malloc(sizeof(Player));
    if (global_player != NULL) {
        global_player->score = 7;
        global_player->speed = 1.5F;
    }
}

void use_player(void)
{
    Player *local = global_player;
    if (local != NULL) {
        local->score += 1;
    }
}

void escape_player(void)
{
    foreign_retain(global_player);
}

Player *return_new_player(void)
{
    return calloc(1U, sizeof(Player));
}

void *unknown_allocation(size_t bytes)
{
    return malloc(bytes);
}

void destroy_player(void)
{
    free(global_player);
    global_player = NULL;
}
