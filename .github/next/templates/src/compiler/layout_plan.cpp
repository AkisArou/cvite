#include "cvite/semantic_index.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>

namespace cvite::semantic {
namespace {

bool samePrefixField(const FieldInfo &old_field, const FieldInfo &new_field)
{
    return old_field.name == new_field.name &&
        old_field.canonical_type == new_field.canonical_type &&
        old_field.bit_offset == new_field.bit_offset &&
        old_field.bit_size == new_field.bit_size &&
        old_field.alignment == new_field.alignment &&
        old_field.bit_width == new_field.bit_width &&
        old_field.bit_field == new_field.bit_field &&
        old_field.flexible_array == new_field.flexible_array;
}

MigrationPlan reject(
    const RecordInfo &old_record,
    const RecordInfo &new_record,
    const std::string &reason)
{
    MigrationPlan plan;
    plan.record = new_record.name.empty() ? old_record.name : new_record.name;
    plan.reason = reason;
    return plan;
}

} // namespace

MigrationPlan planManagedAppendOnly(
    const RecordInfo &old_record,
    const RecordInfo &new_record)
{
    if (old_record.id != new_record.id) {
        return reject(old_record, new_record, "record identity changed");
    }
    if (old_record.is_union || new_record.is_union) {
        return reject(old_record, new_record, "unions are not migratable");
    }
    if (old_record.size <= 0 || new_record.size <= old_record.size) {
        return reject(
            old_record,
            new_record,
            "new record is not a strict size extension");
    }
    if (old_record.alignment <= 0 ||
        old_record.alignment != new_record.alignment) {
        return reject(old_record, new_record, "record alignment changed");
    }
    if (new_record.fields.size() <= old_record.fields.size()) {
        return reject(old_record, new_record, "no append-only field was added");
    }

    MigrationPlan plan;
    plan.record = new_record.name;
    plan.old_size = static_cast<std::size_t>(old_record.size);
    plan.new_size = static_cast<std::size_t>(new_record.size);
    plan.alignment = static_cast<std::size_t>(new_record.alignment);

    for (std::size_t index = 0; index < old_record.fields.size(); ++index) {
        const FieldInfo &old_field = old_record.fields[index];
        const FieldInfo &new_field = new_record.fields[index];
        if (!samePrefixField(old_field, new_field)) {
            return reject(
                old_record,
                new_record,
                "an existing field changed type, position, or shape");
        }
        if (old_field.bit_field || old_field.flexible_array ||
            old_field.bit_offset < 0 || old_field.bit_size <= 0 ||
            (old_field.bit_offset % 8) != 0 ||
            (old_field.bit_size % 8) != 0) {
            return reject(
                old_record,
                new_record,
                "an existing field is not byte-addressable");
        }
        const std::size_t offset =
            static_cast<std::size_t>(old_field.bit_offset / 8);
        const std::size_t size =
            static_cast<std::size_t>(old_field.bit_size / 8);
        if (offset > plan.old_size || size > plan.old_size - offset ||
            offset > plan.new_size || size > plan.new_size - offset) {
            return reject(old_record, new_record, "field range exceeds record");
        }
        plan.copies.push_back(
            {old_field.name, offset, offset, size});
    }

    for (std::size_t index = old_record.fields.size();
         index < new_record.fields.size();
         ++index) {
        const FieldInfo &field = new_record.fields[index];
        if (field.bit_field || field.flexible_array || field.bit_offset < 0 ||
            field.bit_size <= 0 || (field.bit_offset % 8) != 0 ||
            (field.bit_size % 8) != 0) {
            return reject(
                old_record,
                new_record,
                "an appended field is not byte-addressable");
        }
        if (field.bit_offset < old_record.size * 8) {
            return reject(
                old_record,
                new_record,
                "an added field reuses old padding instead of extending the record");
        }
        const std::size_t offset =
            static_cast<std::size_t>(field.bit_offset / 8);
        const std::size_t size =
            static_cast<std::size_t>(field.bit_size / 8);
        if (offset > plan.new_size || size > plan.new_size - offset) {
            return reject(
                old_record,
                new_record,
                "appended field range exceeds the new record");
        }
    }

    std::sort(
        plan.copies.begin(),
        plan.copies.end(),
        [](const CopyOperation &left, const CopyOperation &right) {
            return left.old_offset < right.old_offset;
        });
    for (std::size_t index = 1; index < plan.copies.size(); ++index) {
        const CopyOperation &previous = plan.copies[index - 1U];
        const CopyOperation &current = plan.copies[index];
        if (previous.old_offset + previous.size > current.old_offset) {
            return reject(
                old_record,
                new_record,
                "field byte ranges overlap");
        }
    }

    plan.safe = true;
    plan.reason =
        "safe only for storage owned by CVite with controlled pointer provenance";
    return plan;
}

bool executeMigration(
    const MigrationPlan &plan,
    const void *old_data,
    std::size_t old_size,
    void *new_data,
    std::size_t new_size,
    std::string &error)
{
    if (!plan.safe) {
        error = "cannot execute an unsafe migration plan: " + plan.reason;
        return false;
    }
    if (old_data == nullptr || new_data == nullptr) {
        error = "migration buffers must not be null";
        return false;
    }
    if (old_size != plan.old_size || new_size != plan.new_size) {
        error = "migration buffer sizes do not match the semantic plan";
        return false;
    }

    std::memset(new_data, 0, new_size);
    const auto *old_bytes = static_cast<const unsigned char *>(old_data);
    auto *new_bytes = static_cast<unsigned char *>(new_data);
    for (const CopyOperation &operation : plan.copies) {
        if (operation.old_offset > old_size ||
            operation.size > old_size - operation.old_offset ||
            operation.new_offset > new_size ||
            operation.size > new_size - operation.new_offset) {
            error = "migration operation exceeds its buffer";
            return false;
        }
        std::memcpy(
            new_bytes + operation.new_offset,
            old_bytes + operation.old_offset,
            operation.size);
    }
    error.clear();
    return true;
}

std::string formatPlan(const MigrationPlan &plan)
{
    std::ostringstream output;
    output << (plan.safe ? "managed migration candidate" : "restart required")
           << " for " << plan.record << '\n';
    output << "  " << plan.reason << '\n';
    if (plan.safe) {
        output << "  size: " << plan.old_size << " -> " << plan.new_size
               << " bytes; alignment: " << plan.alignment << '\n';
        output << "  zero destination [0, " << plan.new_size << ")\n";
        for (const CopyOperation &operation : plan.copies) {
            output << "  copy " << operation.field << " old+"
                   << operation.old_offset << " -> new+"
                   << operation.new_offset << ", " << operation.size
                   << " bytes\n";
        }
    }
    return output.str();
}

} // namespace cvite::semantic
