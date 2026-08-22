#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct Player {
    int score;
    float speed;
} Player;

static Player *player;

int update_player(void)
{
    if (player == NULL) {
        player = malloc(sizeof(Player));
        if (player == NULL) {
            return -1;
        }
        player->score = 0;
        player->speed = 1.0F;
    }
    player->score += 1;
    return player->score;
}

int main(void)
{
    const struct timespec delay = {0, 75000000L};
    for (int iteration = 0; iteration < 240; ++iteration) {
        printf("VALUE=%d\n", update_player());
        fflush(stdout);
        (void)nanosleep(&delay, NULL);
    }
    free(player);
    return 0;
}
