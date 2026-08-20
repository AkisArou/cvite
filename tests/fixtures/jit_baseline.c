int cvite_jit_add(int value)
{
    return value + 4;
}

int main(int argc, char **argv)
{
    (void)argv;
    return cvite_jit_add(argc);
}
