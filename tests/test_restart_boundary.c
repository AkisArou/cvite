#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(CONDITION)                                                        \
    do {                                                                        \
        if (!(CONDITION)) {                                                     \
            (void)fprintf(                                                      \
                stderr,                                                         \
                "CHECK failed at %s:%d: %s\n",                                \
                __FILE__,                                                       \
                __LINE__,                                                       \
                #CONDITION);                                                    \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

int main(int argc, char **argv)
{
    cvite_orc_loader *loader = NULL;
    cvite_orc_generation baseline = CVITE_ORC_GENERATION_INVALID;
    cvite_orc_generation candidate = CVITE_ORC_GENERATION_INVALID;
    cvite_program_main program_main = NULL;
    cvite_patch patch;
    cvite_error error;
    cvite_status status;

    CHECK(argc == 3);
    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &baseline, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_baseline(
              loader, baseline, &program_main, &error) == CVITE_STATUS_OK);
    CHECK(program_main != NULL);

    CHECK(cvite_orc_loader_stage_object(
              loader, argv[2], &candidate, &error) == CVITE_STATUS_OK);
    status = cvite_orc_loader_prepare_patch(
        loader, candidate, 0U, 1U, &patch, &error);
    CHECK(status == CVITE_STATUS_RESTART_REQUIRED);
    CHECK(error.status == CVITE_STATUS_RESTART_REQUIRED);
    CHECK(strstr(error.message, "thread-local storage") != NULL);
    CHECK(strstr(error.message, "restart required") != NULL);
    CHECK(cvite_orc_loader_discard_generation(
              loader, candidate, &error) == CVITE_STATUS_OK);

    cvite_orc_loader_destroy(loader);
    return EXIT_SUCCESS;
}
