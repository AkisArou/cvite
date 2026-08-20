#include "cvite/host.h"
#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(CONDITION)                                                        \
    do {                                                                        \
        if (!(CONDITION)) {                                                      \
            (void)fprintf(                                                       \
                stderr,                                                          \
                "CHECK failed at %s:%d: %s\n",                                  \
                __FILE__,                                                       \
                __LINE__,                                                       \
                #CONDITION);                                                     \
            return EXIT_FAILURE;                                                 \
        }                                                                       \
    } while (0)

int main(int argc, char **argv)
{
    cvite_orc_loader *loader = NULL;
    cvite_orc_generation generation = CVITE_ORC_GENERATION_INVALID;
    cvite_program_main program_main = NULL;
    cvite_host_function function;
    cvite_error error;
    char *program_argv[] = {(char *)"jit-baseline", (char *)"one", NULL};

    CHECK(argc == 2);
    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &generation, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_baseline(
              loader, generation, &program_main, &error) == CVITE_STATUS_OK);
    CHECK(program_main != NULL);
    CHECK(cvite_host_find_function(
              "cvite_jit_add", &function, &error) == CVITE_STATUS_OK);
    CHECK(cvite_host_generation() == 0U);
    CHECK(program_main(2, program_argv) == 6);
    CHECK(cvite_host_generation() == 0U);

    cvite_orc_loader_destroy(loader);
    return EXIT_SUCCESS;
}
