#include "cvite/managed_refresh.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>

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

struct OldPlayer final {
    int score;
    float speed;
};

struct NewPlayer final {
    int score;
    float speed;
    int health;
};

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "managed-refresh test failure: " << message << '\n';
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

bool allocate(
    cvite_managed_domain *domain,
    cvite_id type,
    int score,
    float speed,
    cvite_managed_object &object,
    void *&tracked)
{
    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    void *address = nullptr;
    if (cvite_managed_allocate(
            domain,
            type,
            sizeof(OldPlayer),
            alignof(OldPlayer),
            &object,
            &address,
            &error) != CVITE_MANAGED_OK) {
        std::cerr << error.message << '\n';
        return false;
    }
    auto *player = static_cast<OldPlayer *>(address);
    player->score = score;
    player->speed = speed;
    tracked = nullptr;
    if (cvite_managed_track_pointer(
            domain, &tracked, object, 0U, &error) != CVITE_MANAGED_OK) {
        std::cerr << error.message << '\n';
        return false;
    }
    return true;
}

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

    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    cvite_managed_domain *domain = nullptr;
    if (!require(
            cvite_managed_domain_create(&domain, &error) == CVITE_MANAGED_OK,
            error.message)) {
        return 1;
    }

    const cvite_id type = {UINT64_C(0x7100), UINT64_C(0x7200)};
    cvite_managed_object first = CVITE_MANAGED_OBJECT_INVALID;
    cvite_managed_object second = CVITE_MANAGED_OBJECT_INVALID;
    void *first_pointer = nullptr;
    void *second_pointer = nullptr;
    if (!allocate(domain, type, 10, 1.5F, first, first_pointer) ||
        !allocate(domain, type, 20, 2.5F, second, second_pointer)) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    const void *first_before = first_pointer;
    const void *second_before = second_pointer;

    const cvite::refresh::ManagedRefreshResult migration =
        cvite::refresh::migrateRecord(
            domain, type, old_index, append_index, "Player");
    if (!require(migration.applied, migration.diagnostic) ||
        !require(!migration.restart_required, "safe migration requested restart") ||
        !require(migration.migrated_objects == 2U, "object count mismatch") ||
        !require(first_pointer != first_before, "first pointer was not rewritten") ||
        !require(second_pointer != second_before, "second pointer was not rewritten")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    const auto *first_value = static_cast<const NewPlayer *>(first_pointer);
    const auto *second_value = static_cast<const NewPlayer *>(second_pointer);
    if (!require(first_value->score == 10, "first score changed") ||
        !require(std::fabs(first_value->speed - 1.5F) < 0.001F, "first speed changed") ||
        !require(first_value->health == 0, "first health not initialized") ||
        !require(second_value->score == 20, "second score changed") ||
        !require(second_value->health == 0, "second health not initialized")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    const void *stable_pointer = first_pointer;
    const cvite::refresh::ManagedRefreshResult incompatible =
        cvite::refresh::migrateRecord(
            domain, type, old_index, break_index, "Player");
    if (!require(!incompatible.applied, "incompatible layout was applied") ||
        !require(incompatible.restart_required, "incompatible layout did not restart") ||
        !require(first_pointer == stable_pointer, "rejected layout moved storage")) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    cvite_managed_domain *escaped_domain = nullptr;
    if (!require(
            cvite_managed_domain_create(&escaped_domain, &error) ==
                CVITE_MANAGED_OK,
            error.message)) {
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    const cvite_id escaped_type = {UINT64_C(0x7300), UINT64_C(0x7400)};
    cvite_managed_object escaped = CVITE_MANAGED_OBJECT_INVALID;
    void *escaped_pointer = nullptr;
    if (!allocate(
            escaped_domain,
            escaped_type,
            30,
            3.5F,
            escaped,
            escaped_pointer) ||
        !require(
            cvite_managed_mark_escaped(
                escaped_domain, escaped, &error) == CVITE_MANAGED_OK,
            error.message)) {
        cvite_managed_domain_destroy(escaped_domain);
        cvite_managed_domain_destroy(domain);
        return 1;
    }
    const void *escaped_before = escaped_pointer;
    const cvite::refresh::ManagedRefreshResult escaped_result =
        cvite::refresh::migrateRecord(
            escaped_domain,
            escaped_type,
            old_index,
            append_index,
            "Player");
    if (!require(!escaped_result.applied, "escaped object was migrated") ||
        !require(escaped_result.restart_required, "escaped object did not restart") ||
        !require(
            escaped_result.managed_status == CVITE_MANAGED_POINTER_ESCAPED,
            "escaped status was not preserved") ||
        !require(escaped_pointer == escaped_before, "escaped pointer moved")) {
        cvite_managed_domain_destroy(escaped_domain);
        cvite_managed_domain_destroy(domain);
        return 1;
    }

    cvite_managed_domain_destroy(escaped_domain);
    cvite_managed_domain_destroy(domain);
    std::cout << "semantic managed refresh coordinator checks passed\n";
    return 0;
}
