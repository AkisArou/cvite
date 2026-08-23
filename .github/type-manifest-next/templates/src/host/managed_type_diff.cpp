#include "cvite/managed_type_diff.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace cvite::refresh {
namespace {

bool sameId(
    std::uint64_t left_high,
    std::uint64_t left_low,
    std::uint64_t right_high,
    std::uint64_t right_low)
{
    return left_high == right_high && left_low == right_low;
}

bool sameField(
    const cvite_managed_field_record &old_field,
    const cvite_managed_field_record &new_field)
{
    return sameId(
               old_field.id_high,
               old_field.id_low,
               new_field.id_high,
               new_field.id_low) &&
        sameId(
               old_field.type_high,
               old_field.type_low,
               new_field.type_high,
               new_field.type_low) &&
        old_field.offset == new_field.offset &&
        old_field.size == new_field.size &&
        old_field.flags == new_field.flags;
}

std::string typeName(const cvite_managed_type_record &type)
{
    return type.debug_name == nullptr || type.debug_name[0] == '\0'
        ? std::string("<anonymous managed type>")
        : std::string(type.debug_name);
}

ManagedTypePlan reject(
    const cvite_managed_type_record &type,
    const std::string &reason)
{
    ManagedTypePlan result;
    result.diagnostic = typeName(type) + ": restart required: " + reason;
    return result;
}

} // namespace

const cvite_managed_type_record *findManagedType(
    const cvite_managed_type_manifest &manifest,
    std::uint64_t id_high,
    std::uint64_t id_low)
{
    if (manifest.schema != CVITE_MANAGED_TYPE_MANIFEST_SCHEMA ||
        (manifest.type_count > 0U && manifest.types == nullptr)) {
        return nullptr;
    }
    for (std::uint64_t index = 0U; index < manifest.type_count; ++index) {
        const cvite_managed_type_record &type = manifest.types[index];
        if (sameId(type.id_high, type.id_low, id_high, id_low)) {
            return &type;
        }
    }
    return nullptr;
}

ManagedTypePlan planManagedTypeEvolution(
    const cvite_managed_type_record &old_type,
    const cvite_managed_type_record &new_type)
{
    if (!sameId(
            old_type.id_high,
            old_type.id_low,
            new_type.id_high,
            new_type.id_low)) {
        return reject(new_type, "type identity changed");
    }
    if ((old_type.field_count > 0U && old_type.fields == nullptr) ||
        (new_type.field_count > 0U && new_type.fields == nullptr)) {
        return reject(new_type, "field table is invalid");
    }

    const bool exact_layout = sameId(
        old_type.layout_high,
        old_type.layout_low,
        new_type.layout_high,
        new_type.layout_low);
    bool exact_fields = old_type.field_count == new_type.field_count;
    if (exact_fields) {
        for (std::uint64_t index = 0U; index < old_type.field_count; ++index) {
            if (!sameField(old_type.fields[index], new_type.fields[index])) {
                exact_fields = false;
                break;
            }
        }
    }
    if (exact_layout && exact_fields && old_type.size == new_type.size &&
        old_type.alignment == new_type.alignment) {
        ManagedTypePlan result;
        result.compatibility = ManagedTypeCompatibility::identical;
        result.diagnostic = typeName(new_type) + ": layout is identical";
        return result;
    }

    if (old_type.size == 0U || new_type.size <= old_type.size) {
        return reject(new_type, "new layout is not a strict size extension");
    }
    if (old_type.alignment == 0U ||
        old_type.alignment != new_type.alignment) {
        return reject(new_type, "alignment changed");
    }
    if (new_type.field_count <= old_type.field_count) {
        return reject(new_type, "no field was appended");
    }

    ManagedTypePlan result;
    result.copies.reserve(static_cast<std::size_t>(old_type.field_count));
    for (std::uint64_t index = 0U; index < old_type.field_count; ++index) {
        const cvite_managed_field_record &old_field = old_type.fields[index];
        const cvite_managed_field_record &new_field = new_type.fields[index];
        if (!sameField(old_field, new_field)) {
            return reject(
                new_type,
                "an existing field changed identity, type, offset, size, or shape");
        }
        if (old_field.flags != 0U || old_field.size == 0U ||
            old_field.offset > old_type.size ||
            old_field.size > old_type.size - old_field.offset ||
            old_field.offset > new_type.size ||
            old_field.size > new_type.size - old_field.offset) {
            return reject(new_type, "an existing field is not safely copyable");
        }
        result.copies.push_back({
            static_cast<std::size_t>(old_field.offset),
            static_cast<std::size_t>(new_field.offset),
            static_cast<std::size_t>(old_field.size),
        });
    }

    for (std::uint64_t index = old_type.field_count;
         index < new_type.field_count;
         ++index) {
        const cvite_managed_field_record &field = new_type.fields[index];
        if (field.flags != 0U || field.size == 0U ||
            field.offset < old_type.size || field.offset > new_type.size ||
            field.size > new_type.size - field.offset) {
            return reject(
                new_type,
                "an appended field is not a byte-addressable tail extension");
        }
    }

    result.compatibility = ManagedTypeCompatibility::append_only;
    result.byte_plan.old_size = static_cast<std::size_t>(old_type.size);
    result.byte_plan.new_size = static_cast<std::size_t>(new_type.size);
    result.byte_plan.alignment =
        static_cast<std::size_t>(new_type.alignment);
    result.byte_plan.copies = result.copies.data();
    result.byte_plan.copy_count = result.copies.size();
    std::ostringstream diagnostic;
    diagnostic << typeName(new_type) << ": append-only managed migration, "
               << old_type.size << " -> " << new_type.size << " bytes, "
               << old_type.field_count << " fields preserved, "
               << (new_type.field_count - old_type.field_count)
               << " fields zero-initialized";
    result.diagnostic = diagnostic.str();
    return result;
}

} // namespace cvite::refresh
