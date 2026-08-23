#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CVITE_REAL_CLANG_PATH
#error "CVITE_REAL_CLANG_PATH is required"
#endif
#ifndef CVITE_ALLOCATION_INDEX_PATH
#error "CVITE_ALLOCATION_INDEX_PATH is required"
#endif
#ifndef CVITE_ALLOCATION_ANNOTATOR_PATH
#error "CVITE_ALLOCATION_ANNOTATOR_PATH is required"
#endif

namespace {

int runProcess(const std::vector<std::string> &arguments)
{
    if (arguments.empty()) {
        return 127;
    }
    std::vector<char *> raw;
    raw.reserve(arguments.size() + 1U);
    for (const std::string &argument : arguments) {
        raw.push_back(const_cast<char *>(argument.c_str()));
    }
    raw.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "cvite-clang: fork failed: " << std::strerror(errno)
                  << '\n';
        return 127;
    }
    if (child == 0) {
        execv(raw[0], raw.data());
        std::fprintf(
            stderr,
            "cvite-clang: exec failed for %s: %s\n",
            raw[0],
            std::strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        std::cerr << "cvite-clang: waitpid failed: " << std::strerror(errno)
                  << '\n';
        return 127;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 127;
}

bool trueEnvironment(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr &&
        (std::strcmp(value, "1") == 0 ||
         std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "yes") == 0);
}

bool sourceExtension(const std::filesystem::path &path)
{
    const std::string extension = path.extension().string();
    return extension == ".c" || extension == ".i";
}

struct Invocation final {
    bool emits_llvm_ir = false;
    std::string output;
    std::string source;
    std::vector<std::string> semantic_arguments;
};

bool consumesNext(const std::string &argument)
{
    return argument == "-o" || argument == "-MF" || argument == "-MT" ||
        argument == "-MQ" || argument == "-MJ" || argument == "-Xclang" ||
        argument == "-mllvm";
}

bool dependencyFlag(const std::string &argument)
{
    return argument == "-MD" || argument == "-MMD" || argument == "-MP" ||
        argument == "-MG" || argument == "-MM" || argument == "-M";
}

Invocation analyze(int argc, char **argv)
{
    Invocation result;
    bool saw_emit_llvm = false;
    bool saw_assembly = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-emit-llvm") {
            saw_emit_llvm = true;
            continue;
        }
        if (argument == "-S") {
            saw_assembly = true;
            continue;
        }
        if (argument == "-c") {
            continue;
        }
        if (argument == "-o" && index + 1 < argc) {
            result.output = argv[++index];
            continue;
        }
        if (argument.size() > 2U && argument.rfind("-o", 0U) == 0U) {
            result.output = argument.substr(2U);
            continue;
        }
        if (dependencyFlag(argument)) {
            continue;
        }
        if ((argument == "-MF" || argument == "-MT" ||
             argument == "-MQ" || argument == "-MJ") &&
            index + 1 < argc) {
            ++index;
            continue;
        }
        if ((argument == "-Xclang" || argument == "-mllvm") &&
            index + 1 < argc) {
            ++index;
            continue;
        }
        if (!argument.empty() && argument[0] != '-') {
            const std::filesystem::path path(argument);
            std::error_code error;
            if (sourceExtension(path) && std::filesystem::exists(path, error)) {
                result.source = argument;
                continue;
            }
        }
        result.semantic_arguments.push_back(argument);
    }
    result.emits_llvm_ir = saw_emit_llvm && saw_assembly &&
        !result.output.empty() && !result.source.empty();
    return result;
}

std::string temporaryPath(
    const std::string &output,
    const std::string &suffix)
{
    return output + ".cvite." + std::to_string((long long)getpid()) + suffix;
}

void removeQuietly(const std::string &path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

int annotate(const Invocation &invocation)
{
    const std::string index_path =
        temporaryPath(invocation.output, ".alloc.cvaidx");
    const std::string annotated_path =
        temporaryPath(invocation.output, ".annotated.ll");

    std::vector<std::string> index_command = {
        CVITE_ALLOCATION_INDEX_PATH,
        "index",
        "--source",
        invocation.source,
        "--output",
        index_path,
        "--",
    };
    index_command.insert(
        index_command.end(),
        invocation.semantic_arguments.begin(),
        invocation.semantic_arguments.end());
    int status = runProcess(index_command);
    if (status != 0) {
        removeQuietly(index_path);
        removeQuietly(annotated_path);
        return status;
    }

    status = runProcess({
        CVITE_ALLOCATION_ANNOTATOR_PATH,
        "--input",
        invocation.output,
        "--index",
        index_path,
        "--output",
        annotated_path,
    });
    if (status != 0) {
        removeQuietly(index_path);
        removeQuietly(annotated_path);
        return status;
    }

    std::error_code error;
    std::filesystem::rename(annotated_path, invocation.output, error);
    if (error) {
        std::filesystem::copy_file(
            annotated_path,
            invocation.output,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (!error) {
            removeQuietly(annotated_path);
        }
    }
    removeQuietly(index_path);
    if (error) {
        std::cerr << "cvite-clang: cannot publish annotated LLVM IR: "
                  << error.message() << '\n';
        removeQuietly(annotated_path);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> clang_arguments;
    clang_arguments.reserve(static_cast<std::size_t>(argc));
    clang_arguments.emplace_back(CVITE_REAL_CLANG_PATH);
    for (int index = 1; index < argc; ++index) {
        clang_arguments.emplace_back(argv[index]);
    }

    const Invocation invocation = analyze(argc, argv);
    const int status = runProcess(clang_arguments);
    if (status != 0 || !invocation.emits_llvm_ir ||
        trueEnvironment("CVITE_DISABLE_MANAGED_ALLOCATIONS")) {
        return status;
    }
    return annotate(invocation);
}
