typedef struct app_state {
    int value;
} app_state;

static int helper(int value)
{
    return value + 3;
}

int app_update(app_state *state, int delta)
{
    state->value += helper(delta);
    return state->value;
}

int main(void)
{
    app_state state = {0};
    return app_update(&state, 1) == 4 ? 0 : 1;
}
