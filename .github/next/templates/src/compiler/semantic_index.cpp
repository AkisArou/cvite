#include "cvite/semantic_index.hpp"

#include <clang-c/Index.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cvite::semantic {
namespace {

std::string takeString(CXString value)
{
    const char *text = clang_getCString(value);
    std::string result = text == nullptr ? std::string() : std::string(text);
    clang_disposeString(value);
    return result;
}

std::string normalizedPath(const std::string &path)
{
    if (path.empty()) {
        return path;
    }
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::path(path).lexically_normal().string()
                 : normalized.string();
}

struct Location final {
    std::string path;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

Location cursorLocation(CXCursor cursor)
{
    CXFile file = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    clang_getSpellingLocation(
        clang_getCursorLocation(cursor),
        &file,
        &line,
        &column,
        &offset);
    (void)offset;
    return {
        normalizedPath(file == nullptr ? std::string()
                                       : takeString(clang_getFileName(file))),
        static_cast<std::uint32_t>(line),
        static_cast<std::uint32_t>(column),
    };
}

std::string fallbackId(
    CXCursor cursor,
    const Location &location,
    const std::string &name)
{
    std::ostringstream output;
    output << location.path << ':' << location.line << ':' << location.column
           << ':' << static_cast<unsigned>(clang_getCursorKind(cursor)) << ':'
           << name;
    return output.str();
}

std::string cursorId(
    CXCursor cursor,
    const Location &location,
    const std::string &name)
{
    std::string id = takeString(clang_getCursorUSR(cursor));
    return id.empty() ? fallbackId(cursor, location, name) : id;
}

std::int64_t typeSize(CXType type)
{
    const long long value = clang_Type_getSizeOf(type);
    return value < 0 ? -1 : static_cast<std::int64_t>(value);
}

std::int64_t typeAlignment(CXType type)
{
    const long long value = clang_Type_getAlignOf(type);
    return value < 0 ? -1 : static_cast<std::int64_t>(value);
}

std::string recordDisplayName(
    CXCursor cursor,
    const Location &location)
{
    std::string name = takeString(clang_getCursorSpelling(cursor));
    if (!name.empty()) {
        return name;
    }
    std::ostringstream output;
    output << "(anonymous at " << location.path << ':' << location.line << ':'
           << location.column << ')';
    return output.str();
}

struct FieldContext final {
    RecordInfo *record = nullptr;
};

CXChildVisitResult visitField(
    CXCursor cursor,
    CXCursor,
    CXClientData client_data)
{
    if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
        return CXChildVisit_Continue;
    }

    auto &context = *static_cast<FieldContext *>(client_data);
    const Location location = cursorLocation(cursor);
    const std::string name = takeString(clang_getCursorSpelling(cursor));
    const CXType type = clang_getCursorType(cursor);
    const CXType canonical = clang_getCanonicalType(type);
    const long long offset = clang_Cursor_getOffsetOfField(cursor);
    const bool bit_field = clang_Cursor_isBitField(cursor) != 0;
    const int bit_width = bit_field ? clang_getFieldDeclBitWidth(cursor) : -1;
    const bool flexible = type.kind == CXType_IncompleteArray ||
        type.kind == CXType_VariableArray;

    context.record->fields.push_back(FieldInfo{
        cursorId(cursor, location, name),
        name,
        takeString(clang_getTypeSpelling(canonical)),
        offset < 0 ? -1 : static_cast<std::int64_t>(offset),
        typeSize(type) < 0 ? -1 : typeSize(type) * 8,
        typeAlignment(type),
        bit_width,
        bit_field,
        flexible,
    });
    return CXChildVisit_Continue;
}

struct RecordContext final {
    std::map<std::string, RecordInfo> records;
};

CXChildVisitResult visitRecord(
    CXCursor cursor,
    CXCursor,
    CXClientData client_data)
{
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if ((kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl) &&
        clang_isCursorDefinition(cursor) != 0) {
        auto &context = *static_cast<RecordContext *>(client_data);
        const Location location = cursorLocation(cursor);
        const std::string name = recordDisplayName(cursor, location);
        const std::string id = cursorId(cursor, location, name);
        RecordInfo record{
            id,
            name,
            location.path,
            location.line,
            location.column,
            typeSize(clang_getCursorType(cursor)),
            typeAlignment(clang_getCursorType(cursor)),
            kind == CXCursor_UnionDecl,
            {},
        };
        FieldContext fields{&record};
        clang_visitChildren(cursor, visitField, &fields);
        context.records[id] = std::move(record);
    }
    return CXChildVisit_Recurse;
}

std::string diagnosticText(CXDiagnostic diagnostic)
{
    return takeString(clang_formatDiagnostic(
        diagnostic,
        clang_defaultDiagnosticDisplayOptions()));
}

char hexDigit(unsigned value)
{
    return static_cast<char>(value < 10U ? ('0' + value) : ('a' + value - 10U));
}

std::string hexEncode(const std::string &text)
{
    if (text.empty()) {
        return "-";
    }
    std::string output;
    output.reserve(text.size() * 2U);
    for (char character : text) {
        const unsigned byte = static_cast<unsigned>(
            static_cast<unsigned char>(character));
        output.push_back(hexDigit((byte >> 4U) & 0xFU));
        output.push_back(hexDigit(byte & 0xFU));
    }
    return output;
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool hexDecode(
    const std::string &encoded,
    std::string &decoded)
{
    decoded.clear();
    if (encoded == "-") {
        return true;
    }
    if ((encoded.size() % 2U) != 0U) {
        return false;
    }
    decoded.reserve(encoded.size() / 2U);
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        const int high = hexValue(encoded[index]);
        const int low = hexValue(encoded[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::vector<std::string> splitTabs(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(
            start,
            tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1U;
    }
}

template <typename Integer>
bool parseInteger(const std::string &text, Integer &value)
{
    try {
        std::size_t consumed = 0;
        if constexpr (std::numeric_limits<Integer>::is_signed) {
            const long long parsed = std::stoll(text, &consumed, 10);
            if (consumed != text.size()) {
                return false;
            }
            value = static_cast<Integer>(parsed);
        } else {
            const unsigned long long parsed = std::stoull(text, &consumed, 10);
            if (consumed != text.size()) {
                return false;
            }
            value = static_cast<Integer>(parsed);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool sameField(const FieldInfo &left, const FieldInfo &right)
{
    return left.name == right.name &&
        left.canonical_type == right.canonical_type &&
        left.bit_offset == right.bit_offset &&
        left.bit_size == right.bit_size &&
        left.alignment == right.alignment &&
        left.bit_width == right.bit_width &&
        left.bit_field == right.bit_field &&
        left.flexible_array == right.flexible_array;
}

std::string compatibilityName(Compatibility compatibility)
{
    switch (compatibility) {
    case Compatibility::identical:
        return "identical";
    case Compatibility::append_only_candidate:
        return "append-only managed candidate";
    case Compatibility::incompatible:
        return "incompatible";
    }
    return "incompatible";
}

} // namespace

bool buildIndex(
    const std::string &source,
    const std::vector<std::string> &arguments,
    SemanticIndex &index,
    std::string &diagnostics)
{
    diagnostics.clear();
    index = SemanticIndex{};

    std::vector<const char *> raw_arguments;
    raw_arguments.reserve(arguments.size());
    for (const std::string &argument : arguments) {
        raw_arguments.push_back(argument.c_str());
    }

    CXIndex clang_index = clang_createIndex(0, 0);
    if (clang_index == nullptr) {
        diagnostics = "failed to create libclang index";
        return false;
    }

    CXTranslationUnit unit = nullptr;
    const CXErrorCode status = clang_parseTranslationUnit2(
        clang_index,
        source.c_str(),
        raw_arguments.data(),
        static_cast<int>(raw_arguments.size()),
        nullptr,
        0,
        CXTranslationUnit_None,
        &unit);
    if (status != CXError_Success || unit == nullptr) {
        clang_disposeIndex(clang_index);
        diagnostics = "libclang failed to parse " + source;
        return false;
    }

    bool has_error = false;
    const unsigned diagnostic_count = clang_getNumDiagnostics(unit);
    for (unsigned index_value = 0; index_value < diagnostic_count; ++index_value) {
        CXDiagnostic diagnostic = clang_getDiagnostic(unit, index_value);
        if (!diagnostics.empty()) {
            diagnostics.push_back('\n');
        }
        diagnostics += diagnosticText(diagnostic);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity == CXDiagnostic_Error || severity == CXDiagnostic_Fatal) {
            has_error = true;
        }
        clang_disposeDiagnostic(diagnostic);
    }

    CXTargetInfo target = clang_getTranslationUnitTargetInfo(unit);
    if (target != nullptr) {
        index.target = takeString(clang_TargetInfo_getTriple(target));
        clang_TargetInfo_dispose(target);
    }

    RecordContext records;
    clang_visitChildren(clang_getTranslationUnitCursor(unit), visitRecord, &records);
    index.records.reserve(records.records.size());
    for (auto &entry : records.records) {
        index.records.push_back(std::move(entry.second));
    }
    std::sort(
        index.records.begin(),
        index.records.end(),
        [](const RecordInfo &left, const RecordInfo &right) {
            return left.id < right.id;
        });

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(clang_index);
    return !has_error;
}

bool writeIndex(
    const SemanticIndex &index,
    const std::string &path,
    std::string &error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "cannot open semantic index for writing: " + path;
        return false;
    }
    output << "CVITE_SEMANTIC_INDEX_V1\n";
    output << "TARGET\t" << hexEncode(index.target) << '\n';
    for (const RecordInfo &record : index.records) {
        output << "RECORD\t" << hexEncode(record.id) << '\t'
               << hexEncode(record.name) << '\t' << hexEncode(record.source)
               << '\t' << record.line << '\t' << record.column << '\t'
               << record.size << '\t' << record.alignment << '\t'
               << (record.is_union ? 1 : 0) << '\t' << record.fields.size()
               << '\n';
        for (const FieldInfo &field : record.fields) {
            output << "FIELD\t" << hexEncode(field.id) << '\t'
                   << hexEncode(field.name) << '\t'
                   << hexEncode(field.canonical_type) << '\t'
                   << field.bit_offset << '\t' << field.bit_size << '\t'
                   << field.alignment << '\t' << field.bit_width << '\t'
                   << (field.bit_field ? 1 : 0) << '\t'
                   << (field.flexible_array ? 1 : 0) << '\n';
        }
        output << "END\n";
    }
    if (!output) {
        error = "failed while writing semantic index: " + path;
        return false;
    }
    error.clear();
    return true;
}

bool readIndex(
    const std::string &path,
    SemanticIndex &index,
    std::string &error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open semantic index: " + path;
        return false;
    }
    index = SemanticIndex{};
    std::string line;
    if (!std::getline(input, line) || line != "CVITE_SEMANTIC_INDEX_V1") {
        error = "unsupported semantic index schema";
        return false;
    }
    if (!std::getline(input, line)) {
        error = "semantic index has no target record";
        return false;
    }
    auto target_fields = splitTabs(line);
    if (target_fields.size() != 2U || target_fields[0] != "TARGET" ||
        !hexDecode(target_fields[1], index.target)) {
        error = "invalid semantic target record";
        return false;
    }

    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto fields = splitTabs(line);
        if (fields.size() != 10U || fields[0] != "RECORD") {
            error = "invalid semantic record header";
            return false;
        }
        RecordInfo record;
        std::uint64_t field_count = 0;
        int union_value = 0;
        if (!hexDecode(fields[1], record.id) ||
            !hexDecode(fields[2], record.name) ||
            !hexDecode(fields[3], record.source) ||
            !parseInteger(fields[4], record.line) ||
            !parseInteger(fields[5], record.column) ||
            !parseInteger(fields[6], record.size) ||
            !parseInteger(fields[7], record.alignment) ||
            !parseInteger(fields[8], union_value) ||
            !parseInteger(fields[9], field_count)) {
            error = "invalid semantic record values";
            return false;
        }
        record.is_union = union_value != 0;
        for (std::uint64_t field_index = 0; field_index < field_count;
             ++field_index) {
            if (!std::getline(input, line)) {
                error = "truncated semantic field list";
                return false;
            }
            auto field_values = splitTabs(line);
            if (field_values.size() != 10U || field_values[0] != "FIELD") {
                error = "invalid semantic field record";
                return false;
            }
            FieldInfo field;
            int bit_field = 0;
            int flexible = 0;
            if (!hexDecode(field_values[1], field.id) ||
                !hexDecode(field_values[2], field.name) ||
                !hexDecode(field_values[3], field.canonical_type) ||
                !parseInteger(field_values[4], field.bit_offset) ||
                !parseInteger(field_values[5], field.bit_size) ||
                !parseInteger(field_values[6], field.alignment) ||
                !parseInteger(field_values[7], field.bit_width) ||
                !parseInteger(field_values[8], bit_field) ||
                !parseInteger(field_values[9], flexible)) {
                error = "invalid semantic field values";
                return false;
            }
            field.bit_field = bit_field != 0;
            field.flexible_array = flexible != 0;
            record.fields.push_back(std::move(field));
        }
        if (!std::getline(input, line) || line != "END") {
            error = "semantic record is missing END";
            return false;
        }
        index.records.push_back(std::move(record));
    }
    error.clear();
    return true;
}

const RecordInfo *findRecord(
    const SemanticIndex &index,
    const std::string &id_or_name)
{
    for (const RecordInfo &record : index.records) {
        if (record.id == id_or_name || record.name == id_or_name) {
            return &record;
        }
    }
    return nullptr;
}

std::vector<RecordDiff> diffIndexes(
    const SemanticIndex &old_index,
    const SemanticIndex &new_index)
{
    std::vector<RecordDiff> result;
    std::map<std::string, const RecordInfo *> new_records;
    for (const RecordInfo &record : new_index.records) {
        new_records[record.id] = &record;
    }

    for (const RecordInfo &old_record : old_index.records) {
        const auto found = new_records.find(old_record.id);
        if (found == new_records.end()) {
            RecordDiff diff;
            diff.id = old_record.id;
            diff.name = old_record.name;
            diff.old_record = &old_record;
            diff.changes.push_back(
                {ChangeKind::record_removed, {}, "record was removed"});
            result.push_back(std::move(diff));
            continue;
        }

        const RecordInfo &new_record = *found->second;
        new_records.erase(found);
        RecordDiff diff;
        diff.id = old_record.id;
        diff.name = new_record.name;
        diff.old_record = &old_record;
        diff.new_record = &new_record;

        if (old_record.size != new_record.size) {
            std::ostringstream detail;
            detail << old_record.size << " -> " << new_record.size << " bytes";
            diff.changes.push_back(
                {ChangeKind::size_changed, {}, detail.str()});
        }
        if (old_record.alignment != new_record.alignment) {
            std::ostringstream detail;
            detail << old_record.alignment << " -> " << new_record.alignment;
            diff.changes.push_back(
                {ChangeKind::alignment_changed, {}, detail.str()});
        }

        const std::size_t common =
            std::min(old_record.fields.size(), new_record.fields.size());
        bool prefix_equal = true;
        for (std::size_t index_value = 0; index_value < common; ++index_value) {
            const FieldInfo &old_field = old_record.fields[index_value];
            const FieldInfo &new_field = new_record.fields[index_value];
            if (sameField(old_field, new_field)) {
                continue;
            }
            prefix_equal = false;
            if (old_field.name != new_field.name) {
                diff.changes.push_back({
                    ChangeKind::field_shape_changed,
                    old_field.name,
                    "field order/name changed to " + new_field.name,
                });
            }
            if (old_field.canonical_type != new_field.canonical_type) {
                diff.changes.push_back({
                    ChangeKind::field_type_changed,
                    old_field.name,
                    old_field.canonical_type + " -> " +
                        new_field.canonical_type,
                });
            }
            if (old_field.bit_offset != new_field.bit_offset) {
                std::ostringstream detail;
                detail << "bit " << old_field.bit_offset << " -> "
                       << new_field.bit_offset;
                diff.changes.push_back({
                    ChangeKind::field_moved,
                    old_field.name,
                    detail.str(),
                });
            }
        }
        for (std::size_t index_value = common;
             index_value < old_record.fields.size();
             ++index_value) {
            diff.changes.push_back({
                ChangeKind::field_removed,
                old_record.fields[index_value].name,
                "field was removed",
            });
            prefix_equal = false;
        }
        for (std::size_t index_value = common;
             index_value < new_record.fields.size();
             ++index_value) {
            const FieldInfo &field = new_record.fields[index_value];
            std::ostringstream detail;
            detail << field.canonical_type << " @ bit " << field.bit_offset;
            diff.changes.push_back(
                {ChangeKind::field_added, field.name, detail.str()});
        }

        const bool no_special_fields = std::none_of(
            new_record.fields.begin(),
            new_record.fields.end(),
            [](const FieldInfo &field) {
                return field.bit_field || field.flexible_array ||
                    field.bit_offset < 0 || field.bit_size <= 0;
            });
        bool additions_after_old_extent = true;
        for (std::size_t index_value = old_record.fields.size();
             index_value < new_record.fields.size();
             ++index_value) {
            if (new_record.fields[index_value].bit_offset < old_record.size * 8) {
                additions_after_old_extent = false;
            }
        }

        const bool exact = old_record.is_union == new_record.is_union &&
            old_record.size == new_record.size &&
            old_record.alignment == new_record.alignment &&
            old_record.fields.size() == new_record.fields.size() &&
            prefix_equal;
        const bool append_only = !old_record.is_union && !new_record.is_union &&
            old_record.size >= 0 && new_record.size > old_record.size &&
            old_record.alignment == new_record.alignment &&
            new_record.fields.size() > old_record.fields.size() &&
            common == old_record.fields.size() && prefix_equal &&
            additions_after_old_extent && no_special_fields;
        diff.compatibility = exact
            ? Compatibility::identical
            : (append_only ? Compatibility::append_only_candidate
                           : Compatibility::incompatible);
        result.push_back(std::move(diff));
    }

    for (const auto &entry : new_records) {
        RecordDiff diff;
        diff.id = entry.second->id;
        diff.name = entry.second->name;
        diff.new_record = entry.second;
        diff.changes.push_back(
            {ChangeKind::record_added, {}, "record was added"});
        result.push_back(std::move(diff));
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const RecordDiff &left, const RecordDiff &right) {
            return left.id < right.id;
        });
    return result;
}

std::string formatDiff(const RecordDiff &diff)
{
    std::ostringstream output;
    output << (diff.new_record != nullptr && diff.new_record->is_union
                   ? "union "
                   : "struct ")
           << diff.name << ": " << compatibilityName(diff.compatibility)
           << '\n';
    for (const Change &change : diff.changes) {
        output << "  ";
        switch (change.kind) {
        case ChangeKind::field_added:
            output << "+ field ";
            break;
        case ChangeKind::field_removed:
            output << "- field ";
            break;
        case ChangeKind::field_moved:
            output << "~ moved field ";
            break;
        case ChangeKind::field_type_changed:
            output << "~ type of field ";
            break;
        case ChangeKind::size_changed:
            output << "size: ";
            break;
        case ChangeKind::alignment_changed:
            output << "alignment: ";
            break;
        case ChangeKind::record_added:
        case ChangeKind::record_removed:
        case ChangeKind::field_shape_changed:
            output << "~ ";
            break;
        }
        if (!change.field.empty()) {
            output << change.field << ": ";
        }
        output << change.detail << '\n';
    }
    return output.str();
}

} // namespace cvite::semantic
