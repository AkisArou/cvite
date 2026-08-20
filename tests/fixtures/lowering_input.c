typedef struct Pair {
    int first;
    int second;
} Pair;

static int helper(int value)
{
    return value * 2;
}

int add(int left, int right)
{
    return helper(left) + right;
}

Pair make_pair(int first, int second)
{
    Pair pair = {first, second};
    return pair;
}
