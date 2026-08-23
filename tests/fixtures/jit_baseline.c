static int bias = 4;

int cvite_jit_add(int value)
{
    static int calls = 0;
    const int result = value + bias + calls;
    bias += 1;
    calls += 1;
    return result;
}

int main(int argc, char **argv)
{
    (void)argv;
    return cvite_jit_add(argc);
}
