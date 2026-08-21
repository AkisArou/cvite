static _Thread_local int counter;

int app_update(void)
{
    counter += 1;
    return counter;
}

int main(void)
{
    return app_update();
}
