static int finalized;

__attribute__((destructor))
static void finalize_candidate(void)
{
    finalized = 1;
}

int app_update(void)
{
    return finalized;
}

int main(void)
{
    return app_update();
}
