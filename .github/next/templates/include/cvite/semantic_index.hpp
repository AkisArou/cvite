#ifndef CVITE_SEMANTIC_INDEX_HPP
#define CVITE_SEMANTIC_INDEX_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cvite::semantic {

struct FieldInfo final {
    std::string id;
    std::string name;
    std::string canonical_type;
    std::int64_t bit_offset = -1;
    std::int64_t bit_size = -1;
    std::int64_t alignment = -1;
    int bit_width = -1;
    bool bit_field = false;
    bool flexible_array = false;
};

struct RecordInfo final {
    std::string id;
    std::string name;
    std::string source;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::int64_t size = -1;
    std::int64_t alignment = -1;
    bool is_union = false;
    std::vector<FieldInfo> fields;
};

struct SemanticIndex final {
    std::string target;
    std::vector<RecordInfo> records;
};

enum class Compatibility {
    identical,
    append_only_candidate,
    incompatible,
};

enum class ChangeKind {
    record_added,
    record_removed,
    size_changed,
    alignment_changed,
    field_added,
    field_removed,
    field_moved,
    field_type_changed,
    field_shape_changed,
};

struct Change final {
    ChangeKind kind = ChangeKind::field_shape_changed;
    std::string field;
    std::string detail;
};

struct RecordDiff final {
    std::string id;
    std::string name;
    Compatibility compatibility = Compatibility::incompatible;
    const RecordInfo *old_record = nullptr;
    const RecordInfo *new_record = nullptr;
    std::vector<Change> changes;
};

struct CopyOperation final {
    std::string field;
    std::size_t old_offset = 0;
    std::size_t new_offset = 0;
    std::size_t size = 0;
};

struct MigrationPlan final {
    bool safe = false;
    std::string record;
    std::string reason;
    std::size_t old_size = 0;
    std::size_t new_size = 0;
    std::size_t alignment = 0;
    std::vector<CopyOperation> copies;
};

bool buildIndex(
    const std::string &source,
    const std::vector<std::string> &arguments,
    SemanticIndex &index,
    std::string &diagnostics);

bool writeIndex(
    const SemanticIndex &index,
    const std::string &path,
    std::string &error);

bool readIndex(
    const std::string &path,
    SemanticIndex &index,
    std::string &error);

const RecordInfo *findRecord(
    const SemanticIndex &index,
    const std::string &id_or_name);

std::vector<RecordDiff> diffIndexes(
    const SemanticIndex &old_index,
    const SemanticIndex &new_index);

MigrationPlan planManagedAppendOnly(
    const RecordInfo &old_record,
    const RecordInfo &new_record);

bool executeMigration(
    const MigrationPlan &plan,
    const void *old_data,
    std::size_t old_size,
    void *new_data,
    std::size_t new_size,
    std::string &error);

std::string formatDiff(const RecordDiff &diff);
std::string formatPlan(const MigrationPlan &plan);

} // namespace cvite::semantic

#endif
