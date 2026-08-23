#include "cvite/managed_type_diff.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "managed-type-diff test failure: " << message << '\n';
    }
    return condition;
}

cvite_managed_field_record field(
    std::uint64_t id,
    std::uint64_t type,
    std::uint64_t offset,
    std::uint64_t size,
    std::uint64_t flags,
    const char *name)
{
    return {
        UINT64_C(0x1000),
        id,
        UINT64_C(0x2000),
        type,
        offset,
        size,
        flags,
        name,
    };
}

} // namespace

int main()
{
    const cvite_managed_field_record old_fields[] = {
        field(1U, 11U, 0U, 4U, 0U, "score"),
        field(2U, 12U, 4U, 4U, 0U, "speed"),
    };
    const cvite_managed_field_record new_fields[] = {
        old_fields[0],
        old_fields[1],
        field(3U, 13U, 8U, 4U, 0U, "health"),
    };
    const cvite_managed_type_record old_type = {
        UINT64_C(0x3000),
        UINT64_C(0x3001),
        UINT64_C(0x4000),
        UINT64_C(0x4001),
        8U,
        4U,
        2U,
        old_fields,
        "Player",
    };
    const cvite_managed_type_record identical_type = old_type;
    const cvite_managed_type_record new_type = {
        UINT64_C(0x3000),
        UINT64_C(0x3001),
        UINT64_C(0x5000),
        UINT64_C(0x5001),
        12U,
        4U,
        3U,
        new_fields,
        "Player",
    };

    const auto identical =
        cvite::refresh::planManagedTypeEvolution(old_type, identical_type);
    if (!require(
            identical.compatibility ==
                cvite::refresh::ManagedTypeCompatibility::identical,
            identical.diagnostic)) {
        return 1;
    }

    const auto append =
        cvite::refresh::planManagedTypeEvolution(old_type, new_type);
    if (!require(
            append.compatibility ==
                cvite::refresh::ManagedTypeCompatibility::append_only,
            append.diagnostic) ||
        !require(append.copies.size() == 2U, "copy count mismatch") ||
        !require(append.byte_plan.old_size == 8U, "old size mismatch") ||
        !require(append.byte_plan.new_size == 12U, "new size mismatch") ||
        !require(append.byte_plan.copy_count == 2U, "byte-plan count mismatch")) {
        return 1;
    }

    cvite_managed_field_record moved_fields[] = {
        old_fields[1],
        old_fields[0],
        new_fields[2],
    };
    const cvite_managed_type_record moved_type = {
        new_type.id_high,
        new_type.id_low,
        UINT64_C(0x6000),
        UINT64_C(0x6001),
        12U,
        4U,
        3U,
        moved_fields,
        "Player",
    };
    const auto moved =
        cvite::refresh::planManagedTypeEvolution(old_type, moved_type);
    if (!require(
            moved.compatibility ==
                cvite::refresh::ManagedTypeCompatibility::incompatible,
            "reordered fields were accepted")) {
        return 1;
    }

    cvite_managed_field_record bit_fields[] = {
        old_fields[0],
        old_fields[1],
        field(
            4U,
            14U,
            8U,
            4U,
            CVITE_MANAGED_FIELD_BIT_FIELD,
            "flags"),
    };
    const cvite_managed_type_record bit_type = {
        new_type.id_high,
        new_type.id_low,
        UINT64_C(0x7000),
        UINT64_C(0x7001),
        12U,
        4U,
        3U,
        bit_fields,
        "Player",
    };
    const auto bit =
        cvite::refresh::planManagedTypeEvolution(old_type, bit_type);
    if (!require(
            bit.compatibility ==
                cvite::refresh::ManagedTypeCompatibility::incompatible,
            "bit-field extension was accepted")) {
        return 1;
    }

    const cvite_managed_type_record types[] = {old_type, new_type};
    const cvite_managed_type_manifest manifest = {
        CVITE_MANAGED_TYPE_MANIFEST_SCHEMA,
        2U,
        types,
    };
    if (!require(
            cvite::refresh::findManagedType(
                manifest, new_type.id_high, new_type.id_low) == &types[0],
            "manifest lookup did not return first matching type") ||
        !require(
            cvite::refresh::findManagedType(
                manifest, UINT64_C(9), UINT64_C(9)) == nullptr,
            "unknown manifest type was found")) {
        return 1;
    }

    std::cout << "managed type manifest compatibility checks passed\n";
    return 0;
}
