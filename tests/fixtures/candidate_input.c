typedef struct app_state {
    int value;
} app_state;

static int global_bias = 3;

static int helper(int value)
{
    static int calls = 1;
    calls += 1;
    global_bias += 1;
    return value + calls + global_bias;
}

int app_update(app_state *state, int delta)
{
    state->value += helper(delta);
    return state->value;
}

int main(void)
{
    app_state state = {0};
    return app_update(&state, 1) == 7 ? 0 : 1;
}
