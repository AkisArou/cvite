typedef struct counter_state {
    int value;
    int updates;
} counter_state;

extern int cvite_host_multiplier(int value);

int __cvite_candidate_update_g1(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;
    state->value += cvite_host_multiplier(delta);
    state->updates += 1;
    return state->value;
}
