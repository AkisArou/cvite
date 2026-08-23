int filter_alpha(int value)
{
    return value + 10;
}

int filter_beta(int value)
{
    return value * 2;
}

int main(int argc, char **argv)
{
    (void)argv;
    return filter_alpha(argc) + filter_beta(argc);
}
