#ifndef CVITE_COMPILER_SEMANTIC_INDEX_H
#define CVITE_COMPILER_SEMANTIC_INDEX_H

#include <cstdint>
#include <string>
#include <vector>

namespace cvite::semantic {

struct SourceLocation final {
    std::string file;
    unsigned line = 0U;
    unsigned column = 0U;
};

struct Field final {
    std::string id;
    std::string name;
    std::string canonical_type;
    std::int64_t offset_bits = -1;
    std::int64_t size_bytes = -1;
    std::int64_t alignment_bytes = -1;
    int bit_width = -1;
    SourceLocation location;
};

struct Record final {
    std::string id;
    std::string name;
    std::string kind;
    std::int64_t size_bytes = -1;
    std::int64_t alignment_bytes = -1;
    SourceLocation location;
    std::vector<Field> fields;
};

struct Index final {
    std::string source;
    std::string target;
    std::vector<Record> records;
};

struct DiffSummary final {
    bool changed = false;
    bool incompatible = false;
    std::size_t changed_records = 0U;
    std::size_t append_only_records = 0U;
    std::string text;
};

bool buildIndex(
    const std::string &source,
    const std::vector<std::string> &arguments,
    Index &index,
    std::string &error);

bool writeIndex(
    const Index &index,
    const std::string &path,
    std::string &error);

bool readIndex(
    const std::string &path,
    Index &index,
    std::string &error);

DiffSummary diff(const Index &old_index, const Index &new_index);

} // namespace cvite::semantic

#endif
