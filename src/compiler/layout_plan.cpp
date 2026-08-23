#include "layout_plan.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace cvite::semantic {
namespace {

const Record *findRecord(const Index &index, const Record &wanted)
{
    for (const Record &record : index.records) {
        if (record.id == wanted.id) {
            return &record;
        }
    }
    for (const Record &record : index.records) {
        if (record.kind == wanted.kind && record.name == wanted.name) {
            return &record;
        }
    }
    return nullptr;
}

const Field *findField(const Record &record, const Field &wanted)
{
    for (const Field &field : record.fields) {
        if (field.id == wanted.id) {
            return &field;
        }
    }
    for (const Field &field : record.fields) {
        if (field.name == wanted.name) {
            return &field;
        }
    }
    return nullptr;
}

bool checkedOffset(std::int64_t offset_bits, std::size_t &offset_bytes)
{
    if (offset_bits < 0 || offset_bits % 8 != 0) {
        return false;
    }
    const auto unsigned_bits = static_cast<std::uint64_t>(offset_bits);
    const std::uint64_t bytes = unsigned_bits / 8U;
    if (bytes > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    offset_bytes = static_cast<std::size_t>(bytes);
    return true;
}

bool checkedSize(std::int64_t size, std::size_t &converted)
{
    if (size < 0 || static_cast<std::uint64_t>(size) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    converted = static_cast<std::size_t>(size);
    return true;
}

bool exactFieldLayout(const Field &old_field, const Field &new_field)
{
    return old_field.canonical_type == new_field.canonical_type &&
        old_field.offset_bits == new_field.offset_bits &&
        old_field.size_bytes == new_field.size_bytes &&
        old_field.alignment_bytes == new_field.alignment_bytes &&
        old_field.bit_width == new_field.bit_width;
}

RecordMigrationPlan reject(
    RecordMigrationPlan plan,
    std::string reason)
{
    plan.safe_for_managed_storage = false;
    plan.rejection_reason = std::move(reason);
    plan.actions.clear();
    return plan;
}

RecordMigrationPlan planRecord(
    const Record &old_record,
    const Record &new_record)
{
    RecordMigrationPlan plan;
    plan.record_id = old_record.id;
    plan.record_name = old_record.name;
    plan.record_kind = old_record.kind;

    if (!checkedSize(old_record.size_bytes, plan.old_size) ||
        !checkedSize(new_record.size_bytes, plan.new_size) ||
        !checkedSize(old_record.alignment_bytes, plan.alignment)) {
        plan.changed = true;
        return reject(std::move(plan), "record has an unknown or unsupported size/alignment");
    }

    plan.changed = old_record.size_bytes != new_record.size_bytes ||
        old_record.alignment_bytes != new_record.alignment_bytes ||
        old_record.fields.size() != new_record.fields.size();
    if (!plan.changed) {
        for (const Field &old_field : old_record.fields) {
            const Field *new_field = findField(new_record, old_field);
            if (new_field == nullptr || !exactFieldLayout(old_field, *new_field)) {
                plan.changed = true;
                break;
            }
        }
    }
    if (!plan.changed) {
        plan.safe_for_managed_storage = true;
        return plan;
    }

    if (old_record.kind != "struct" || new_record.kind != "struct") {
        return reject(std::move(plan), "only struct records are eligible; union migration is ambiguous");
    }
    if (old_record.alignment_bytes != new_record.alignment_bytes) {
        return reject(std::move(plan), "record alignment changed");
    }
    if (new_record.size_bytes < old_record.size_bytes) {
        return reject(std::move(plan), "record size shrank");
    }

    plan.actions.push_back(MigrationAction{
        MigrationActionKind::ZeroInitialize,
        {},
        {},
        0U,
        0U,
        plan.new_size,
    });

    for (const Field &old_field : old_record.fields) {
        const Field *new_field = findField(new_record, old_field);
        if (new_field == nullptr) {
            return reject(
                std::move(plan),
                "field '" + old_field.name + "' was removed or renamed without a stable identity");
        }
        if (!exactFieldLayout(old_field, *new_field)) {
            return reject(
                std::move(plan),
                "field '" + old_field.name + "' changed type, offset, size, alignment, or bit width");
        }
        if (old_field.bit_width >= 0 || new_field->bit_width >= 0) {
            return reject(
                std::move(plan),
                "bit-field migration is not yet supported");
        }
        std::size_t old_offset = 0U;
        std::size_t new_offset = 0U;
        std::size_t field_size = 0U;
        if (!checkedOffset(old_field.offset_bits, old_offset) ||
            !checkedOffset(new_field->offset_bits, new_offset) ||
            !checkedSize(old_field.size_bytes, field_size)) {
            return reject(
                std::move(plan),
                "field '" + old_field.name + "' has a non-byte or unknown layout");
        }
        if (old_offset > plan.old_size || field_size > plan.old_size - old_offset ||
            new_offset > plan.new_size || field_size > plan.new_size - new_offset) {
            return reject(
                std::move(plan),
                "field '" + old_field.name + "' lies outside its record storage");
        }
        plan.actions.push_back(MigrationAction{
            MigrationActionKind::CopyField,
            old_field.name,
            old_field.canonical_type,
            old_offset,
            new_offset,
            field_size,
        });
    }

    for (const Field &new_field : new_record.fields) {
        if (findField(old_record, new_field) != nullptr) {
            continue;
        }
        if (new_field.bit_width >= 0) {
            return reject(
                std::move(plan),
                "new field '" + new_field.name + "' is a bit-field");
        }
        std::size_t offset = 0U;
        std::size_t size = 0U;
        if (!checkedOffset(new_field.offset_bits, offset) ||
            !checkedSize(new_field.size_bytes, size)) {
            return reject(
                std::move(plan),
                "new field '" + new_field.name + "' has a non-byte or unknown layout");
        }
        if (offset < plan.old_size) {
            return reject(
                std::move(plan),
                "new field '" + new_field.name + "' was inserted into the existing layout");
        }
        if (offset > plan.new_size || size > plan.new_size - offset) {
            return reject(
                std::move(plan),
                "new field '" + new_field.name + "' lies outside the new record storage");
        }
    }

    plan.safe_for_managed_storage = true;
    return plan;
}

} // namespace

MigrationPlan buildMigrationPlan(
    const Index &old_index,
    const Index &new_index)
{
    MigrationPlan result;
    std::map<std::string, bool> matched_new;

    for (const Record &old_record : old_index.records) {
        const Record *new_record = findRecord(new_index, old_record);
        if (new_record == nullptr) {
            RecordMigrationPlan removed;
            removed.record_id = old_record.id;
            removed.record_name = old_record.name;
            removed.record_kind = old_record.kind;
            removed.changed = true;
            removed.safe_for_managed_storage = false;
            removed.rejection_reason = "record was removed";
            result.records.push_back(std::move(removed));
            result.changed = true;
            result.all_changed_records_safe = false;
            continue;
        }
        matched_new[new_record->id] = true;
        RecordMigrationPlan record = planRecord(old_record, *new_record);
        result.changed = result.changed || record.changed;
        if (record.changed && !record.safe_for_managed_storage) {
            result.all_changed_records_safe = false;
        }
        result.records.push_back(std::move(record));
    }

    for (const Record &new_record : new_index.records) {
        if (matched_new[new_record.id]) {
            continue;
        }
        bool exists_by_name = false;
        for (const Record &old_record : old_index.records) {
            exists_by_name = exists_by_name ||
                (old_record.kind == new_record.kind &&
                 old_record.name == new_record.name);
        }
        if (exists_by_name) {
            continue;
        }
        RecordMigrationPlan added;
        added.record_id = new_record.id;
        added.record_name = new_record.name;
        added.record_kind = new_record.kind;
        added.changed = true;
        added.safe_for_managed_storage = true;
        if (!checkedSize(new_record.size_bytes, added.new_size) ||
            !checkedSize(new_record.alignment_bytes, added.alignment)) {
            added.safe_for_managed_storage = false;
            added.rejection_reason = "new record has an unknown layout";
            result.all_changed_records_safe = false;
        }
        result.changed = true;
        result.records.push_back(std::move(added));
    }

    return result;
}

bool applyManagedMigration(
    const RecordMigrationPlan &plan,
    const void *old_storage,
    std::size_t old_storage_size,
    void *new_storage,
    std::size_t new_storage_size,
    std::string &error)
{
    if (!plan.changed || !plan.safe_for_managed_storage) {
        error = plan.changed
            ? "migration plan is not safe for managed storage: " +
                plan.rejection_reason
            : "record layout did not change";
        return false;
    }
    if (old_storage == nullptr || new_storage == nullptr ||
        old_storage_size < plan.old_size ||
        new_storage_size < plan.new_size) {
        error = "migration buffers do not satisfy the plan sizes";
        return false;
    }

    const auto *old_bytes = static_cast<const unsigned char *>(old_storage);
    auto *new_bytes = static_cast<unsigned char *>(new_storage);
    for (const MigrationAction &action : plan.actions) {
        if (action.kind == MigrationActionKind::ZeroInitialize) {
            if (action.new_offset > new_storage_size ||
                action.size > new_storage_size - action.new_offset) {
                error = "zero-initialize action exceeds the destination buffer";
                return false;
            }
            std::memset(new_bytes + action.new_offset, 0, action.size);
            continue;
        }
        if (action.old_offset > old_storage_size ||
            action.size > old_storage_size - action.old_offset ||
            action.new_offset > new_storage_size ||
            action.size > new_storage_size - action.new_offset) {
            error = "copy action for field '" + action.field +
                "' exceeds a migration buffer";
            return false;
        }
        std::memcpy(
            new_bytes + action.new_offset,
            old_bytes + action.old_offset,
            action.size);
    }
    error.clear();
    return true;
}

std::string formatMigrationPlan(const MigrationPlan &plan)
{
    if (!plan.changed) {
        return "no managed-storage migrations required\n";
    }
    std::ostringstream output;
    output << "CVite managed-storage migration plan\n";
    output << "contract: CVite owns the storage address/provenance; this plan "
              "does not authorize arbitrary C pointer-graph migration\n";
    for (const RecordMigrationPlan &record : plan.records) {
        if (!record.changed) {
            continue;
        }
        output << record.record_kind << ' ' << record.record_name << ": ";
        if (!record.safe_for_managed_storage) {
            output << "rejected: " << record.rejection_reason << '\n';
            continue;
        }
        if (record.old_size == 0U) {
            output << "new record; initialize " << record.new_size
                   << " bytes\n";
            continue;
        }
        output << "managed append-only migration, " << record.old_size
               << " -> " << record.new_size << " bytes, alignment "
               << record.alignment << '\n';
        for (const MigrationAction &action : record.actions) {
            if (action.kind == MigrationActionKind::ZeroInitialize) {
                output << "  zero destination [" << action.new_offset << ", "
                       << action.new_offset + action.size << ")\n";
            } else {
                output << "  copy field " << action.field << " ("
                       << action.canonical_type << ") old+"
                       << action.old_offset << " -> new+" << action.new_offset
                       << ", " << action.size << " bytes\n";
            }
        }
    }
    output << "result: "
           << (plan.all_changed_records_safe
                   ? "all changed records have managed-storage plans"
                   : "one or more changed records require restart")
           << '\n';
    return output.str();
}

} // namespace cvite::semantic
