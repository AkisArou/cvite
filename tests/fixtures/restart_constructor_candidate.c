static int initialized;

__attribute__((constructor))
static void initialize_candidate(void)
{
    initialized = 41;
}

int app_update(void)
{
    return initialized + 1;
}

int main(void)
{
    return app_update();
}
