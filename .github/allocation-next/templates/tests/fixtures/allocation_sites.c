#include <stddef.h>
#include <stdlib.h>

typedef struct Player {
    int score;
    float speed;
} Player;

Player *make_player(void)
{
    return malloc(sizeof(Player));
}

Player *make_players(size_t count)
{
    return calloc(count, sizeof(Player));
}

Player *make_cast_player(size_t bytes)
{
    return (Player *)malloc(bytes);
}

void *make_unknown(size_t bytes)
{
    return malloc(bytes);
}

Player *resize_players(Player *players, size_t count)
{
    return realloc(players, count * sizeof(Player));
}
