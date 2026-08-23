static int global_bias = 3;

static int helper(int value)
{
    static int calls = 1;
    calls += 1;
    global_bias += 1;
    return value * 2 + calls + global_bias;
}

int app_update(int value)
{
    return helper(value) + 1;
}

int main(void)
{
    return app_update(3) == 13 ? 0 : 1;
}
