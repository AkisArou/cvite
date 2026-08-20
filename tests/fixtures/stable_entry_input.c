static int helper(int value)
{
    return value * 2;
}

int app_update(int value)
{
    return helper(value) + 1;
}

int main(void)
{
    return app_update(3) == 7 ? 0 : 1;
}
