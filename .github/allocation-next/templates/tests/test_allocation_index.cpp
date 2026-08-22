#include "cvite/allocation_index.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#ifndef CVITE_ALLOCATION_SOURCE
#error "CVITE_ALLOCATION_SOURCE is required"
#endif

namespace {

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "allocation-index test failure: " << message << '\n';
    }
    return condition;
}

} // namespace

int main()
{
    cvite::semantic::AllocationIndex index;
    std::string diagnostics;
    if (!require(
            cvite::semantic::buildAllocationIndex(
                CVITE_ALLOCATION_SOURCE,
                {"-std=c11"},
                index,
                diagnostics),
            diagnostics)) {
        return 1;
    }
    if (!require(index.sites.size() == 5U, "expected five allocation sites")) {
        for (const auto &site : index.sites) {
            std::cerr << cvite::semantic::formatAllocationSite(site) << '\n';
        }
        return 1;
    }

    std::size_t typed_player = 0U;
    std::size_t unknown = 0U;
    std::size_t malloc_count = 0U;
    std::size_t calloc_count = 0U;
    std::size_t realloc_count = 0U;
    bool saw_cast_typed = false;
    for (const auto &site : index.sites) {
        if (site.kind == cvite::semantic::AllocationKind::malloc_call) {
            ++malloc_count;
        } else if (site.kind == cvite::semantic::AllocationKind::calloc_call) {
            ++calloc_count;
        } else if (site.kind == cvite::semantic::AllocationKind::realloc_call) {
            ++realloc_count;
        }
        if (site.confidence ==
            cvite::semantic::AllocationConfidence::unknown) {
            ++unknown;
        } else if (site.type_name.find("Player") != std::string::npos) {
            ++typed_player;
            saw_cast_typed = saw_cast_typed ||
                site.confidence ==
                    cvite::semantic::AllocationConfidence::typed_result;
        }
        if (!require(
                !site.source.empty() && site.line > 0U && site.column > 0U,
                "site has no source location") ||
            !require(!site.id.empty(), "site has no stable identity")) {
            return 1;
        }
    }

    if (!require(malloc_count == 3U, "malloc count mismatch") ||
        !require(calloc_count == 1U, "calloc count mismatch") ||
        !require(realloc_count == 1U, "realloc count mismatch") ||
        !require(typed_player >= 4U, "typed Player allocations were missed") ||
        !require(unknown == 1U, "untyped allocation was not conservative") ||
        !require(saw_cast_typed, "typed-result inference was not exercised")) {
        for (const auto &site : index.sites) {
            std::cerr << cvite::semantic::formatAllocationSite(site) << '\n';
        }
        return 1;
    }

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "cvite-allocation-index-roundtrip.cvaidx";
    std::string error;
    if (!require(
            cvite::semantic::writeAllocationIndex(index, path.string(), error),
            error)) {
        return 1;
    }
    cvite::semantic::AllocationIndex roundtrip;
    if (!require(
            cvite::semantic::readAllocationIndex(
                path.string(), roundtrip, error),
            error) ||
        !require(
            roundtrip.sites.size() == index.sites.size(),
            "roundtrip changed the site count") ||
        !require(
            cvite::semantic::formatAllocationSite(roundtrip.sites.front())
                    .find("confidence=") != std::string::npos,
            "formatted site omits confidence")) {
        return 1;
    }
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    std::cout << "typed allocation index checks passed\n";
    return 0;
}
