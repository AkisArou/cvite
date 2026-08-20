typedef struct counter_state {
    int value;
    int updates;
} counter_state;

int cvite_e2e_update(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;
    state->value += delta;
    state->updates += 1;
    return state->value;
}
