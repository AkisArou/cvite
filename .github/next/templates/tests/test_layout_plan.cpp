#include "cvite/semantic_index.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef CVITE_LAYOUT_OLD_SOURCE
#error "CVITE_LAYOUT_OLD_SOURCE is required"
#endif
#ifndef CVITE_LAYOUT_APPEND_SOURCE
#error "CVITE_LAYOUT_APPEND_SOURCE is required"
#endif
#ifndef CVITE_LAYOUT_BREAK_SOURCE
#error "CVITE_LAYOUT_BREAK_SOURCE is required"
#endif

namespace {

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "layout-plan test failure: " << message << '\n';
    }
    return condition;
}

bool build(
    const std::string &source,
    cvite::semantic::SemanticIndex &index)
{
    std::string diagnostics;
    const bool result = cvite::semantic::buildIndex(
        source, {"-std=c11"}, index, diagnostics);
    if (!result) {
        std::cerr << diagnostics << '\n';
    }
    return result;
}

const cvite::semantic::RecordDiff *findDiff(
    const std::vector<cvite::semantic::RecordDiff> &diffs,
    const std::string &name)
{
    for (const auto &diff : diffs) {
        if (diff.name == name) {
            return &diff;
        }
    }
    return nullptr;
}

struct OldPlayer final {
    int score;
    float speed;
};

struct NewPlayer final {
    int score;
    float speed;
    int health;
};

} // namespace

int main()
{
    cvite::semantic::SemanticIndex old_index;
    cvite::semantic::SemanticIndex append_index;
    cvite::semantic::SemanticIndex break_index;
    if (!build(CVITE_LAYOUT_OLD_SOURCE, old_index) ||
        !build(CVITE_LAYOUT_APPEND_SOURCE, append_index) ||
        !build(CVITE_LAYOUT_BREAK_SOURCE, break_index)) {
        return 1;
    }

    const auto append_diffs =
        cvite::semantic::diffIndexes(old_index, append_index);
    const auto *append_diff = findDiff(append_diffs, "Player");
    if (!require(append_diff != nullptr, "Player append diff was not found") ||
        !require(
            append_diff->compatibility ==
                cvite::semantic::Compatibility::append_only_candidate,
            "append-only record was not classified as a candidate") ||
        !require(
            cvite::semantic::formatDiff(*append_diff).find("health") !=
                std::string::npos,
            "field-level diagnostic does not name the added field")) {
        return 1;
    }

    const auto *old_record = cvite::semantic::findRecord(old_index, "Player");
    const auto *append_record =
        cvite::semantic::findRecord(append_index, "Player");
    if (!require(old_record != nullptr, "old Player was not indexed") ||
        !require(append_record != nullptr, "new Player was not indexed")) {
        return 1;
    }

    const auto plan =
        cvite::semantic::planManagedAppendOnly(*old_record, *append_record);
    if (!require(plan.safe, "append-only managed migration was rejected") ||
        !require(plan.copies.size() == 2U, "unexpected copy operation count")) {
        std::cerr << cvite::semantic::formatPlan(plan);
        return 1;
    }

    const OldPlayer old_value{73, 4.5F};
    NewPlayer new_value{-1, -1.0F, -1};
    std::string error;
    if (!require(
            cvite::semantic::executeMigration(
                plan,
                &old_value,
                sizeof(old_value),
                &new_value,
                sizeof(new_value),
                error),
            error) ||
        !require(new_value.score == 73, "integer field was not preserved") ||
        !require(
            std::fabs(new_value.speed - 4.5F) < 0.001F,
            "floating field was not preserved") ||
        !require(new_value.health == 0, "new field was not zero initialized")) {
        return 1;
    }

    const auto break_diffs =
        cvite::semantic::diffIndexes(old_index, break_index);
    const auto *break_diff = findDiff(break_diffs, "Player");
    if (!require(break_diff != nullptr, "Player incompatible diff was not found") ||
        !require(
            break_diff->compatibility ==
                cvite::semantic::Compatibility::incompatible,
            "reordered fields were not rejected")) {
        return 1;
    }

    const std::filesystem::path roundtrip_path =
        std::filesystem::temp_directory_path() /
        "cvite-semantic-index-roundtrip.cvsidx";
    if (!require(
            cvite::semantic::writeIndex(old_index, roundtrip_path.string(), error),
            error)) {
        return 1;
    }
    cvite::semantic::SemanticIndex roundtrip;
    if (!require(
            cvite::semantic::readIndex(
                roundtrip_path.string(), roundtrip, error),
            error) ||
        !require(
            cvite::semantic::findRecord(roundtrip, "Player") != nullptr,
            "round-tripped index lost Player")) {
        return 1;
    }
    std::error_code remove_error;
    std::filesystem::remove(roundtrip_path, remove_error);

    std::cout << "semantic layout planner checks passed\n";
    return 0;
}
