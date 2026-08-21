#include "cvite/host.h"
#include "cvite/orc_loader.h"

#include <stdio.h>
#include <stdlib.h>

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

static int publish_candidate(
    cvite_orc_loader *loader,
    const char *object_path,
    uint64_t expected_runtime_generation,
    uint64_t candidate_runtime_generation,
    size_t expected_update_count,
    cvite_orc_generation *generation)
{
    cvite_patch patch;
    cvite_error error;

    CHECK(cvite_orc_loader_stage_object(
              loader, object_path, generation, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_patch(
              loader,
              *generation,
              expected_runtime_generation,
              candidate_runtime_generation,
              &patch,
              &error) == CVITE_STATUS_OK);
    CHECK(patch.function_count == expected_update_count);

    if (patch.function_count == 0U) {
        CHECK(cvite_orc_loader_discard_generation(
                  loader, *generation, &error) == CVITE_STATUS_OK);
        return EXIT_SUCCESS;
    }

    CHECK(cvite_host_apply_patch(&patch, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_commit_patch(
              loader, *generation, &error) == CVITE_STATUS_OK);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    cvite_orc_loader *loader = NULL;
    cvite_orc_generation baseline = CVITE_ORC_GENERATION_INVALID;
    cvite_orc_generation first = CVITE_ORC_GENERATION_INVALID;
    cvite_orc_generation duplicate = CVITE_ORC_GENERATION_INVALID;
    cvite_orc_generation second = CVITE_ORC_GENERATION_INVALID;
    cvite_program_main program_main = NULL;
    cvite_error error;
    size_t reclaimed = 0U;
    char *program_argv[] = {(char *)"filter-program", NULL};

    CHECK(argc == 4);
    CHECK(cvite_orc_loader_create(&loader, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_stage_object(
              loader, argv[1], &baseline, &error) == CVITE_STATUS_OK);
    CHECK(cvite_orc_loader_prepare_baseline(
              loader, baseline, &program_main, &error) == CVITE_STATUS_OK);
    CHECK(program_main != NULL);
    CHECK(program_main(1, program_argv) == 4);
    CHECK(cvite_host_generation() == 0U);

    CHECK(publish_candidate(loader, argv[2], 0U, 1U, 1U, &first) ==
        EXIT_SUCCESS);
    CHECK(program_main(1, program_argv) == 13);
    CHECK(cvite_host_generation() == 1U);

    /* Re-staging byte-identical code must produce an empty runtime patch. */
    CHECK(publish_candidate(loader, argv[2], 1U, 2U, 0U, &duplicate) ==
        EXIT_SUCCESS);
    CHECK(program_main(1, program_argv) == 13);
    CHECK(cvite_host_generation() == 1U);

    CHECK(publish_candidate(loader, argv[3], 1U, 2U, 1U, &second) ==
        EXIT_SUCCESS);
    CHECK(program_main(1, program_argv) == 23);
    CHECK(cvite_host_generation() == 2U);

    /* A live call scope blocks removal of the now-retired first generation. */
    __cvite_host_call_enter();
    CHECK(cvite_host_active_call_count() == 1U);
    CHECK(cvite_orc_loader_collect_retired(loader, &reclaimed, &error) ==
        CVITE_STATUS_OK);
    CHECK(reclaimed == 0U);
    __cvite_host_call_leave();
    CHECK(cvite_host_active_call_count() == 0U);

    CHECK(cvite_orc_loader_collect_retired(loader, &reclaimed, &error) ==
        CVITE_STATUS_OK);
    CHECK(reclaimed == 1U);

    CHECK(cvite_orc_loader_discard_generation(loader, first, &error) ==
        CVITE_STATUS_INVALID_ARGUMENT);
    CHECK(cvite_orc_loader_discard_generation(loader, second, &error) ==
        CVITE_STATUS_INVALID_STATE);

    cvite_orc_loader_destroy(loader);
    return EXIT_SUCCESS;
}
