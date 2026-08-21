typedef struct counter_state {
    int value;
    int updates;
} counter_state;

int cvite_e2e_global_bias = 10;

extern int cvite_e2e_multiplier(int value);

int cvite_e2e_update(void *opaque, int delta)
{
    counter_state *state = (counter_state *)opaque;

    state->value +=
        cvite_e2e_multiplier(delta) + cvite_e2e_global_bias;
    state->updates += 1;
    cvite_e2e_global_bias += 1;
    return state->value;
}

int main(void)
{
    return 0;
}
