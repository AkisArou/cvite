#ifndef CVITE_COMPILER_LAYOUT_PLAN_H
#define CVITE_COMPILER_LAYOUT_PLAN_H

#include "semantic_index.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cvite::semantic {

enum class MigrationActionKind {
    CopyField,
    ZeroInitialize,
};

struct MigrationAction final {
    MigrationActionKind kind = MigrationActionKind::ZeroInitialize;
    std::string field;
    std::string canonical_type;
    std::size_t old_offset = 0U;
    std::size_t new_offset = 0U;
    std::size_t size = 0U;
};

struct RecordMigrationPlan final {
    std::string record_id;
    std::string record_name;
    std::string record_kind;
    std::size_t old_size = 0U;
    std::size_t new_size = 0U;
    std::size_t alignment = 0U;
    bool changed = false;
    bool safe_for_managed_storage = false;
    std::string rejection_reason;
    std::vector<MigrationAction> actions;
};

struct MigrationPlan final {
    bool changed = false;
    bool all_changed_records_safe = true;
    std::vector<RecordMigrationPlan> records;
};

MigrationPlan buildMigrationPlan(
    const Index &old_index,
    const Index &new_index);

bool applyManagedMigration(
    const RecordMigrationPlan &plan,
    const void *old_storage,
    std::size_t old_storage_size,
    void *new_storage,
    std::size_t new_storage_size,
    std::string &error);

std::string formatMigrationPlan(const MigrationPlan &plan);

} // namespace cvite::semantic

#endif
