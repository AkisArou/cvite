#include "cvite/semantic_index.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(const char *program)
{
    std::cerr
        << "usage:\n"
        << "  " << program
        << " index --source file.c --output file.cvsidx [-- clang-options...]\n"
        << "  " << program << " diff --old old.cvsidx --new new.cvsidx\n"
        << "  " << program
        << " plan --old old.cvsidx --new new.cvsidx --record NameOrUSR\n";
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

    cvite::semantic::SemanticIndex semantic_index;
    std::string diagnostics;
    if (!cvite::semantic::buildIndex(
            source, arguments, semantic_index, diagnostics)) {
        std::cerr << diagnostics << '\n';
        return 1;
    }
    if (!diagnostics.empty()) {
        std::cerr << diagnostics << '\n';
    }
    std::string error;
    if (!cvite::semantic::writeIndex(semantic_index, output, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "indexed " << semantic_index.records.size() << " records\n";
    return 0;
}

bool loadPair(
    int argc,
    char **argv,
    cvite::semantic::SemanticIndex &old_index,
    cvite::semantic::SemanticIndex &new_index)
{
    std::string old_path;
    std::string new_path;
    if (!valueAfter(argc, argv, "--old", old_path) ||
        !valueAfter(argc, argv, "--new", new_path)) {
        return false;
    }
    std::string error;
    if (!cvite::semantic::readIndex(old_path, old_index, error)) {
        std::cerr << error << '\n';
        return false;
    }
    if (!cvite::semantic::readIndex(new_path, new_index, error)) {
        std::cerr << error << '\n';
        return false;
    }
    return true;
}

int diffCommand(int argc, char **argv)
{
    cvite::semantic::SemanticIndex old_index;
    cvite::semantic::SemanticIndex new_index;
    if (!loadPair(argc, argv, old_index, new_index)) {
        return 2;
    }
    bool incompatible = false;
    for (const cvite::semantic::RecordDiff &diff :
         cvite::semantic::diffIndexes(old_index, new_index)) {
        if (diff.compatibility != cvite::semantic::Compatibility::identical) {
            std::cout << cvite::semantic::formatDiff(diff);
        }
        incompatible = incompatible ||
            diff.compatibility == cvite::semantic::Compatibility::incompatible;
    }
    return incompatible ? 3 : 0;
}

int planCommand(int argc, char **argv)
{
    cvite::semantic::SemanticIndex old_index;
    cvite::semantic::SemanticIndex new_index;
    if (!loadPair(argc, argv, old_index, new_index)) {
        return 2;
    }
    std::string record_name;
    if (!valueAfter(argc, argv, "--record", record_name)) {
        return 2;
    }
    const cvite::semantic::RecordInfo *old_record =
        cvite::semantic::findRecord(old_index, record_name);
    const cvite::semantic::RecordInfo *new_record =
        cvite::semantic::findRecord(new_index, record_name);
    if (old_record == nullptr || new_record == nullptr) {
        std::cerr << "record not found in both semantic indexes: "
                  << record_name << '\n';
        return 1;
    }
    const cvite::semantic::MigrationPlan plan =
        cvite::semantic::planManagedAppendOnly(*old_record, *new_record);
    std::cout << cvite::semantic::formatPlan(plan);
    return plan.safe ? 0 : 3;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    int result = 2;
    if (command == "index") {
        result = indexCommand(argc, argv);
    } else if (command == "diff") {
        result = diffCommand(argc, argv);
    } else if (command == "plan") {
        result = planCommand(argc, argv);
    } else {
        usage(argv[0]);
    }
    return result;
}
