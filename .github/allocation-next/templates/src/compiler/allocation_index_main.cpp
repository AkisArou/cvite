#include "cvite/allocation_index.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(const char *program)
{
    std::cerr
        << "usage:\n"
        << "  " << program
        << " index --source file.c --output file.cvaidx [-- clang-options...]\n"
        << "  " << program << " show --index file.cvaidx\n";
}

bool valueAfter(
    int argc,
    char **argv,
    const std::string &option,
    std::string &value)
{
    for (int index = 2; index + 1 < argc; ++index) {
        if (argv[index] == option) {
            value = argv[index + 1];
            return true;
        }
    }
    return false;
}

int indexCommand(int argc, char **argv)
{
    std::string source;
    std::string output;
    if (!valueAfter(argc, argv, "--source", source) ||
        !valueAfter(argc, argv, "--output", output)) {
        return 2;
    }

    std::vector<std::string> arguments;
    bool after_separator = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (after_separator) {
            arguments.push_back(argument);
        } else if (argument == "--") {
            after_separator = true;
        } else if ((argument == "--source" || argument == "--output") &&
                   index + 1 < argc) {
            ++index;
        }
    }

    cvite::semantic::AllocationIndex allocation_index;
    std::string diagnostics;
    if (!cvite::semantic::buildAllocationIndex(
            source, arguments, allocation_index, diagnostics)) {
        std::cerr << diagnostics << '\n';
        return 1;
    }
    if (!diagnostics.empty()) {
        std::cerr << diagnostics << '\n';
    }

    std::string error;
    if (!cvite::semantic::writeAllocationIndex(
            allocation_index, output, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::size_t typed = 0U;
    for (const auto &site : allocation_index.sites) {
        if (site.confidence !=
            cvite::semantic::AllocationConfidence::unknown) {
            ++typed;
        }
    }
    std::cout << "indexed " << allocation_index.sites.size()
              << " allocation sites; " << typed << " typed\n";
    return 0;
}

int showCommand(int argc, char **argv)
{
    std::string path;
    if (!valueAfter(argc, argv, "--index", path)) {
        return 2;
    }
    cvite::semantic::AllocationIndex allocation_index;
    std::string error;
    if (!cvite::semantic::readAllocationIndex(path, allocation_index, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "target=" << allocation_index.target << '\n';
    for (const auto &site : allocation_index.sites) {
        std::cout << cvite::semantic::formatAllocationSite(site) << '\n';
    }
    return 0;
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
    if (command == "show") {
        return showCommand(argc, argv);
    }
    usage(argv[0]);
    return 2;
}
