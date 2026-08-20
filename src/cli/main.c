#include "cvite/version.h"

#if defined(CVITE_RUNNER_ENABLED)
#include "run.h"
#endif

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "Usage: cvite <command>\n"
        "\n"
        "Commands:\n"
        "  doctor       print development-toolchain capabilities\n"
        "  run <path>   run and watch a C source or simple project\n"
        "  version      print the CVite version\n");
}

static int run_doctor(void)
{
    (void)printf("CVite %s\n", CVITE_VERSION_STRING);
#if defined(__linux__)
    (void)printf("platform: linux\n");
#elif defined(__APPLE__)
    (void)printf("platform: macos\n");
#elif defined(_WIN32)
    (void)printf("platform: windows\n");
#else
    (void)printf("platform: unknown\n");
#endif

#if defined(__x86_64__) || defined(_M_X64)
    (void)printf("architecture: x86_64\n");
#elif defined(__aarch64__) || defined(_M_ARM64)
    (void)printf("architecture: arm64\n");
#else
    (void)printf("architecture: unsupported-bootstrap-target\n");
#endif

    (void)printf("runtime core: available\n");
#if defined(CVITE_RUNNER_ENABLED)
    cvite_run_print_doctor(stdout);
#else
    (void)printf("compiler service: not linked in this build\n");
#endif
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) {
        (void)printf("cvite %s\n", CVITE_VERSION_STRING);
        return 0;
    }

    if (strcmp(argv[1], "doctor") == 0) {
        return run_doctor();
    }

    if (strcmp(argv[1], "run") == 0) {
#if defined(CVITE_RUNNER_ENABLED)
        return cvite_run_command(argc - 2, argv + 2);
#else
        if (argc < 3) {
            print_usage(stderr);
            return 2;
        }
        (void)fprintf(
            stderr,
            "cvite: 'run' requires a build with the LLVM pass and ORC loader\n"
            "project: %s\n",
            argv[2]);
        return 3;
#endif
    }

    print_usage(stderr);
    return 2;
}
