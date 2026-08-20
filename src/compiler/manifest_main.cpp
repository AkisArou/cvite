#include <clang-c/Index.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

#include "manifest_support.ipp"
#include "manifest_collect.ipp"
#include "manifest_json.ipp"

} // namespace

int main(int argc, char **argv)
{
    std::string error;
    const std::optional<Options> parsed = parse_options(argc, argv, error);
    if (!parsed.has_value()) {
        if (!error.empty()) {
            std::cerr << "cvite-manifest: " << error << '\n';
            print_usage(std::cerr);
            return 2;
        }
        return 0;
    }

    const Options &options = *parsed;
    TranslationUnitManifest manifest;
    std::string compiler_version;
    if (!build_manifest(options, manifest, compiler_version, error)) {
        std::cerr << "cvite-manifest: " << error << '\n';
        return 1;
    }

    if (options.output == fs::path("-")) {
        write_manifest(std::cout, options, manifest, compiler_version);
        return 0;
    }

    std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cvite-manifest: could not open output file: "
                  << options.output << '\n';
        return 1;
    }
    write_manifest(output, options, manifest, compiler_version);
    if (!output) {
        std::cerr << "cvite-manifest: failed while writing: "
                  << options.output << '\n';
        return 1;
    }
    return 0;
}
