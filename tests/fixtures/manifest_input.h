#ifndef CVITE_MANIFEST_INPUT_H
#define CVITE_MANIFEST_INPUT_H

#define CVITE_FIELD(TYPE, NAME) TYPE NAME

typedef struct Player {
    float x;
    float y;
    CVITE_FIELD(int, health);
} Player;

extern int external_counter;
int game_update(Player *player, int delta);

#endif
