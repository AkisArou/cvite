typedef struct counter_state {
    int value;
    int updates;
} counter_state;

extern int cvite_e2e_multiplier(int value);

int cvite_e2e_update(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;
    state->value += cvite_e2e_multiplier(delta);
    state->updates += 1;
    return state->value;
}
