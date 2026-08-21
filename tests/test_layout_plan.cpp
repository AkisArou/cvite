#include "layout_plan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

cvite::semantic::Field field(
    std::string id,
    std::string name,
    std::string type,
    std::int64_t offset_bits,
    std::int64_t size,
    std::int64_t alignment)
{
    return {
        std::move(id),
        std::move(name),
        std::move(type),
        offset_bits,
        size,
        alignment,
        -1,
        {},
    };
}

cvite::semantic::Record oldPlayer()
{
    return {
        "player-id",
        "Player",
        "struct",
        8,
        4,
        {},
        {
            field("score-id", "score", "int", 0, 4, 4),
            field("speed-id", "speed", "float", 32, 4, 4),
        },
    };
}

cvite::semantic::Record appendedPlayer()
{
    return {
        "player-id",
        "Player",
        "struct",
        12,
        4,
        {},
        {
            field("score-id", "score", "int", 0, 4, 4),
            field("speed-id", "speed", "float", 32, 4, 4),
            field("health-id", "health", "int", 64, 4, 4),
        },
    };
}

cvite::semantic::Record incompatiblePlayer()
{
    return {
        "player-id",
        "Player",
        "struct",
        16,
        8,
        {},
        {
            field("score-id", "score", "int", 0, 4, 4),
            field("speed-id", "speed", "double", 64, 8, 8),
        },
    };
}

bool expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "layout-plan test failure: " << message << '\n';
    }
    return condition;
}

} // namespace

int main()
{
    const cvite::semantic::Index old_index{
        "old.c", "x86_64-pc-linux-gnu", {oldPlayer()}};
    const cvite::semantic::Index new_index{
        "new.c", "x86_64-pc-linux-gnu", {appendedPlayer()}};
    const cvite::semantic::MigrationPlan migration =
        cvite::semantic::buildMigrationPlan(old_index, new_index);
    if (!expect(migration.changed, "append-only record was not changed") ||
        !expect(
            migration.all_changed_records_safe,
            "append-only record was not accepted for managed storage") ||
        !expect(migration.records.size() == 1U, "unexpected record count")) {
        return 1;
    }

    struct OldPlayer {
        std::int32_t score;
        float speed;
    } old_player{73, 2.5F};
    std::array<unsigned char, 12U> new_bytes;
    new_bytes.fill(0xA5U);
    std::string error;
    if (!expect(
            cvite::semantic::applyManagedMigration(
                migration.records.front(),
                &old_player,
                sizeof(old_player),
                new_bytes.data(),
                new_bytes.size(),
                error),
            error.c_str())) {
        return 1;
    }

    std::int32_t score = 0;
    float speed = 0.0F;
    std::int32_t health = -1;
    std::memcpy(&score, new_bytes.data(), sizeof(score));
    std::memcpy(&speed, new_bytes.data() + 4U, sizeof(speed));
    std::memcpy(&health, new_bytes.data() + 8U, sizeof(health));
    if (!expect(score == 73, "score was not preserved") ||
        !expect(speed == 2.5F, "speed was not preserved") ||
        !expect(health == 0, "new health field was not zero initialized")) {
        return 1;
    }

    const cvite::semantic::Index incompatible_index{
        "bad.c", "x86_64-pc-linux-gnu", {incompatiblePlayer()}};
    const cvite::semantic::MigrationPlan incompatible =
        cvite::semantic::buildMigrationPlan(old_index, incompatible_index);
    if (!expect(
            !incompatible.all_changed_records_safe,
            "type/alignment change was accepted") ||
        !expect(
            incompatible.records.size() == 1U &&
                !incompatible.records.front().safe_for_managed_storage,
            "incompatible record did not carry a rejection")) {
        return 1;
    }

    const std::string formatted =
        cvite::semantic::formatMigrationPlan(migration);
    if (!expect(
            formatted.find("copy field score") != std::string::npos,
            "formatted plan omitted score") ||
        !expect(
            formatted.find("zero destination") != std::string::npos,
            "formatted plan omitted zero initialization") ||
        !expect(
            formatted.find("does not authorize arbitrary C pointer-graph") !=
                std::string::npos,
            "formatted plan omitted safety contract")) {
        return 1;
    }

    return 0;
}
