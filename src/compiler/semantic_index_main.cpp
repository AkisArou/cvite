#include "semantic_index.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(const char *program)
{
    std::cerr
        << "usage:\n"
        << "  " << program
        << " index --source <file.c> --output <file.cvsidx> [-- <clang args...>]\n"
        << "  " << program
        << " diff --old <old.cvsidx> --new <new.cvsidx> [--fail-on-change]\n";
}

bool takeOption(
    int argc,
    char **argv,
    int &index,
    const std::string &name,
    std::string &value)
{
    if (std::string(argv[index]) != name || index + 1 >= argc) {
        return false;
    }
    value = argv[++index];
    return true;
}

int indexCommand(int argc, char **argv)
{
    std::string source;
    std::string output;
    std::vector<std::string> clang_arguments;
    bool clang_mode = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (clang_mode) {
            clang_arguments.push_back(argument);
        } else if (argument == "--") {
            clang_mode = true;
        } else if (takeOption(argc, argv, index, "--source", source)) {
        } else if (takeOption(argc, argv, index, "--output", output)) {
        } else {
            std::cerr << "unknown index option: " << argument << '\n';
            return 2;
        }
    }
    if (source.empty() || output.empty()) {
        usage(argv[0]);
        return 2;
    }
    cvite::semantic::Index semantic_index;
    std::string error;
    if (!cvite::semantic::buildIndex(
            source, clang_arguments, semantic_index, error)) {
        std::cerr << error;
        if (!error.empty() && error.back() != '\n') {
            std::cerr << '\n';
        }
        return 3;
    }
    if (!cvite::semantic::writeIndex(semantic_index, output, error)) {
        std::cerr << error << '\n';
        return 3;
    }
    return 0;
}

int diffCommand(int argc, char **argv)
{
    std::string old_path;
    std::string new_path;
    bool fail_on_change = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (takeOption(argc, argv, index, "--old", old_path)) {
        } else if (takeOption(argc, argv, index, "--new", new_path)) {
        } else if (argument == "--fail-on-change") {
            fail_on_change = true;
        } else {
            std::cerr << "unknown diff option: " << argument << '\n';
            return 2;
        }
    }
    if (old_path.empty() || new_path.empty()) {
        usage(argv[0]);
        return 2;
    }
    cvite::semantic::Index old_index;
    cvite::semantic::Index new_index;
    std::string error;
    if (!cvite::semantic::readIndex(old_path, old_index, error) ||
        !cvite::semantic::readIndex(new_path, new_index, error)) {
        std::cerr << error << '\n';
        return 3;
    }
    const cvite::semantic::DiffSummary result =
        cvite::semantic::diff(old_index, new_index);
    std::cout << result.text;
    return fail_on_change && result.changed ? 4 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    if (command == "index") {
        return indexCommand(argc, argv);
    }
    if (command == "diff") {
        return diffCommand(argc, argv);
    }
    usage(argv[0]);
    return 2;
}
