#include "layout_plan.h"
#include "semantic_index.h"

#include <iostream>
#include <string>

namespace {

void usage(const char *program)
{
    std::cerr
        << "usage: " << program
        << " --old <old.cvsidx> --new <new.cvsidx> [--require-safe]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string old_path;
    std::string new_path;
    bool require_safe = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--old" && index + 1 < argc) {
            old_path = argv[++index];
        } else if (argument == "--new" && index + 1 < argc) {
            new_path = argv[++index];
        } else if (argument == "--require-safe") {
            require_safe = true;
        } else {
            usage(argv[0]);
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

    const cvite::semantic::MigrationPlan plan =
        cvite::semantic::buildMigrationPlan(old_index, new_index);
    std::cout << cvite::semantic::formatMigrationPlan(plan);
    return require_safe && !plan.all_changed_records_safe ? 5 : 0;
}
