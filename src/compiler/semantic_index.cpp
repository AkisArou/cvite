#include "semantic_index.h"

#include <clang-c/Index.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cvite::semantic {
namespace {

constexpr std::string_view kSchema = "cvite-semantic-index-v1";

std::string takeString(CXString value)
{
    const char *text = clang_getCString(value);
    std::string result = text == nullptr ? std::string() : std::string(text);
    clang_disposeString(value);
    return result;
}

SourceLocation cursorLocation(CXCursor cursor)
{
    CXFile file = nullptr;
    unsigned line = 0U;
    unsigned column = 0U;
    unsigned offset = 0U;
    clang_getSpellingLocation(
        clang_getCursorLocation(cursor), &file, &line, &column, &offset);
    (void)offset;
    return {
        file == nullptr ? std::string() : takeString(clang_getFileName(file)),
        line,
        column,
    };
}

std::string escape(std::string_view input)
{
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const char character : input) {
        const auto value = static_cast<unsigned char>(character);
        if (value == static_cast<unsigned char>('%') ||
            value == static_cast<unsigned char>('\t') ||
            value == static_cast<unsigned char>('\n') ||
            value == static_cast<unsigned char>('\r')) {
            output << '%' << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(value);
        } else {
            output << character;
        }
    }
    return output.str();
}

std::optional<unsigned> hexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned>(value - 'A' + 10);
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned>(value - 'a' + 10);
    }
    return std::nullopt;
}

bool unescape(std::string_view input, std::string &output)
{
    output.clear();
    output.reserve(input.size());
    for (std::size_t index = 0U; index < input.size(); ++index) {
        if (input[index] != '%') {
            output.push_back(input[index]);
            continue;
        }
        if (index + 2U >= input.size()) {
            return false;
        }
        const std::optional<unsigned> high = hexDigit(input[index + 1U]);
        const std::optional<unsigned> low = hexDigit(input[index + 2U]);
        if (!high.has_value() || !low.has_value()) {
            return false;
        }
        output.push_back(static_cast<char>((*high << 4U) | *low));
        index += 2U;
    }
    return true;
}

std::vector<std::string_view> splitTabs(std::string_view line)
{
    std::vector<std::string_view> columns;
    std::size_t start = 0U;
    for (;;) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string_view::npos) {
            columns.push_back(line.substr(start));
            return columns;
        }
        columns.push_back(line.substr(start, separator - start));
        start = separator + 1U;
    }
}

template <typename Integer>
bool parseInteger(std::string_view text, Integer &value)
{
    std::string owned(text);
    char *end = nullptr;
    errno = 0;
    if constexpr (std::is_signed_v<Integer>) {
        const long long parsed = std::strtoll(owned.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0') {
            return false;
        }
        value = static_cast<Integer>(parsed);
    } else {
        const unsigned long long parsed = std::strtoull(owned.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0') {
            return false;
        }
        value = static_cast<Integer>(parsed);
    }
    return true;
}

std::string identity(CXCursor cursor, const SourceLocation &location)
{
    std::string result = takeString(clang_getCursorUSR(cursor));
    if (!result.empty()) {
        return result;
    }
    std::ostringstream fallback;
    fallback << "location:" << location.file << ':' << location.line << ':'
             << location.column << ':' << takeString(clang_getCursorSpelling(cursor));
    return fallback.str();
}

std::string displayName(
    CXCursor cursor,
    CXType type,
    const SourceLocation &location)
{
    std::string result = takeString(clang_getCursorSpelling(cursor));
    if (result.empty()) {
        result = takeString(clang_getTypeSpelling(type));
    }
    if (result.empty()) {
        std::ostringstream anonymous;
        anonymous << "<anonymous@" << location.file << ':' << location.line
                  << ':' << location.column << '>';
        result = anonymous.str();
    }
    return result;
}

struct FieldVisitor final {
    Record *record = nullptr;
    std::size_t ordinal = 0U;
};

CXChildVisitResult visitField(
    CXCursor cursor,
    CXCursor parent,
    CXClientData client_data)
{
    (void)parent;
    if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
        return CXChildVisit_Continue;
    }
    auto &visitor = *static_cast<FieldVisitor *>(client_data);
    const SourceLocation location = cursorLocation(cursor);
    CXType type = clang_getCanonicalType(clang_getCursorType(cursor));
    std::string name = takeString(clang_getCursorSpelling(cursor));
    if (name.empty()) {
        name = "<anonymous-field-" + std::to_string(visitor.ordinal) + ">";
    }
    std::string field_id = takeString(clang_getCursorUSR(cursor));
    if (field_id.empty()) {
        field_id = visitor.record->id + "::field:" + name + ':' +
            std::to_string(visitor.ordinal);
    }
    visitor.record->fields.push_back(Field{
        std::move(field_id),
        std::move(name),
        takeString(clang_getTypeSpelling(type)),
        clang_Cursor_getOffsetOfField(cursor),
        clang_Type_getSizeOf(type),
        clang_Type_getAlignOf(type),
        clang_Cursor_isBitField(cursor) != 0
            ? clang_getFieldDeclBitWidth(cursor)
            : -1,
        location,
    });
    ++visitor.ordinal;
    return CXChildVisit_Continue;
}

struct IndexVisitor final {
    Index *index = nullptr;
};

CXChildVisitResult visitDeclaration(
    CXCursor cursor,
    CXCursor parent,
    CXClientData client_data)
{
    (void)parent;
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if ((kind != CXCursor_StructDecl && kind != CXCursor_UnionDecl) ||
        clang_isCursorDefinition(cursor) == 0) {
        return CXChildVisit_Recurse;
    }
    if (clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)) != 0) {
        return CXChildVisit_Continue;
    }

    auto &visitor = *static_cast<IndexVisitor *>(client_data);
    CXType type = clang_getCanonicalType(clang_getCursorType(cursor));
    const std::int64_t size = clang_Type_getSizeOf(type);
    const std::int64_t alignment = clang_Type_getAlignOf(type);
    if (size < 0 || alignment < 0) {
        return CXChildVisit_Recurse;
    }
    const SourceLocation location = cursorLocation(cursor);
    Record record{
        identity(cursor, location),
        displayName(cursor, type, location),
        kind == CXCursor_UnionDecl ? "union" : "struct",
        size,
        alignment,
        location,
        {},
    };
    FieldVisitor field_visitor{&record, 0U};
    clang_visitChildren(cursor, visitField, &field_visitor);
    std::sort(
        record.fields.begin(),
        record.fields.end(),
        [](const Field &left, const Field &right) {
            if (left.offset_bits != right.offset_bits) {
                return left.offset_bits < right.offset_bits;
            }
            return left.id < right.id;
        });
    visitor.index->records.push_back(std::move(record));
    return CXChildVisit_Recurse;
}

std::string diagnostics(CXTranslationUnit translation_unit)
{
    std::ostringstream output;
    const unsigned count = clang_getNumDiagnostics(translation_unit);
    for (unsigned index = 0U; index < count; ++index) {
        CXDiagnostic diagnostic = clang_getDiagnostic(translation_unit, index);
        output << takeString(clang_formatDiagnostic(
            diagnostic, clang_defaultDiagnosticDisplayOptions()))
               << '\n';
        clang_disposeDiagnostic(diagnostic);
    }
    return output.str();
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

bool sameField(const Field &old_field, const Field &new_field)
{
    return old_field.canonical_type == new_field.canonical_type &&
        old_field.offset_bits == new_field.offset_bits &&
        old_field.size_bytes == new_field.size_bytes &&
        old_field.alignment_bytes == new_field.alignment_bytes &&
        old_field.bit_width == new_field.bit_width;
}

std::string fieldDescription(const Field &field)
{
    std::ostringstream output;
    output << field.canonical_type << " @ bit " << field.offset_bits;
    if (field.size_bytes >= 0) {
        output << " (" << field.size_bytes << " bytes)";
    }
    if (field.bit_width >= 0) {
        output << ", bit-width " << field.bit_width;
    }
    return output.str();
}

} // namespace

bool buildIndex(
    const std::string &source,
    const std::vector<std::string> &arguments,
    Index &index,
    std::string &error)
{
    index = Index{};
    index.source = source;
    std::vector<const char *> raw_arguments;
    raw_arguments.reserve(arguments.size());
    for (const std::string &argument : arguments) {
        raw_arguments.push_back(argument.c_str());
    }

    CXIndex clang_index = clang_createIndex(0, 0);
    if (clang_index == nullptr) {
        error = "could not create a libclang index";
        return false;
    }
    CXTranslationUnit translation_unit = nullptr;
    const CXErrorCode parse_status = clang_parseTranslationUnit2(
        clang_index,
        source.c_str(),
        raw_arguments.data(),
        static_cast<int>(raw_arguments.size()),
        nullptr,
        0U,
        CXTranslationUnit_KeepGoing,
        &translation_unit);
    if (parse_status != CXError_Success || translation_unit == nullptr) {
        error = "libclang could not parse '" + source + "'";
        clang_disposeIndex(clang_index);
        return false;
    }

    bool has_error = false;
    const unsigned diagnostic_count = clang_getNumDiagnostics(translation_unit);
    for (unsigned diagnostic_index = 0U;
         diagnostic_index < diagnostic_count;
         ++diagnostic_index) {
        CXDiagnostic diagnostic =
            clang_getDiagnostic(translation_unit, diagnostic_index);
        has_error = has_error ||
            clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error;
        clang_disposeDiagnostic(diagnostic);
    }
    if (has_error) {
        error = diagnostics(translation_unit);
        clang_disposeTranslationUnit(translation_unit);
        clang_disposeIndex(clang_index);
        return false;
    }

    CXTargetInfo target_info = clang_getTranslationUnitTargetInfo(translation_unit);
    if (target_info != nullptr) {
        index.target = takeString(clang_TargetInfo_getTriple(target_info));
        clang_TargetInfo_dispose(target_info);
    }

    IndexVisitor visitor{&index};
    clang_visitChildren(
        clang_getTranslationUnitCursor(translation_unit),
        visitDeclaration,
        &visitor);
    std::sort(
        index.records.begin(),
        index.records.end(),
        [](const Record &left, const Record &right) {
            return left.id < right.id;
        });
    index.records.erase(
        std::unique(
            index.records.begin(),
            index.records.end(),
            [](const Record &left, const Record &right) {
                return left.id == right.id;
            }),
        index.records.end());

    clang_disposeTranslationUnit(translation_unit);
    clang_disposeIndex(clang_index);
    error.clear();
    return true;
}

bool writeIndex(
    const Index &index,
    const std::string &path,
    std::string &error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open semantic index output '" + path + "'";
        return false;
    }
    output << kSchema << '\n';
    output << "S\t" << escape(index.source) << '\t' << escape(index.target)
           << '\n';
    for (const Record &record : index.records) {
        output << "R\t" << escape(record.id) << '\t' << escape(record.kind)
               << '\t' << escape(record.name) << '\t' << record.size_bytes
               << '\t' << record.alignment_bytes << '\t'
               << escape(record.location.file) << '\t' << record.location.line
               << '\t' << record.location.column << '\n';
        for (const Field &field : record.fields) {
            output << "F\t" << escape(record.id) << '\t' << escape(field.id)
                   << '\t' << escape(field.name) << '\t'
                   << escape(field.canonical_type) << '\t' << field.offset_bits
                   << '\t' << field.size_bytes << '\t' << field.alignment_bytes
                   << '\t' << field.bit_width << '\t'
                   << escape(field.location.file) << '\t' << field.location.line
                   << '\t' << field.location.column << '\n';
        }
    }
    if (!output) {
        error = "could not write semantic index '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

bool readIndex(
    const std::string &path,
    Index &index,
    std::string &error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open semantic index '" + path + "'";
        return false;
    }
    index = Index{};
    std::string line;
    if (!std::getline(input, line) || line != kSchema) {
        error = "unsupported semantic index schema in '" + path + "'";
        return false;
    }
    std::map<std::string, std::size_t> record_positions;
    std::size_t line_number = 1U;
    while (std::getline(input, line)) {
        ++line_number;
        const std::vector<std::string_view> columns = splitTabs(line);
        if (columns.empty() || columns[0].empty()) {
            continue;
        }
        if (columns[0] == "S") {
            if (columns.size() != 3U ||
                !unescape(columns[1], index.source) ||
                !unescape(columns[2], index.target)) {
                error = "invalid semantic source row at line " +
                    std::to_string(line_number);
                return false;
            }
            continue;
        }
        if (columns[0] == "R") {
            if (columns.size() != 9U) {
                error = "invalid semantic record row at line " +
                    std::to_string(line_number);
                return false;
            }
            Record record;
            if (!unescape(columns[1], record.id) ||
                !unescape(columns[2], record.kind) ||
                !unescape(columns[3], record.name) ||
                !parseInteger(columns[4], record.size_bytes) ||
                !parseInteger(columns[5], record.alignment_bytes) ||
                !unescape(columns[6], record.location.file) ||
                !parseInteger(columns[7], record.location.line) ||
                !parseInteger(columns[8], record.location.column)) {
                error = "invalid semantic record values at line " +
                    std::to_string(line_number);
                return false;
            }
            record_positions[record.id] = index.records.size();
            index.records.push_back(std::move(record));
            continue;
        }
        if (columns[0] == "F") {
            if (columns.size() != 12U) {
                error = "invalid semantic field row at line " +
                    std::to_string(line_number);
                return false;
            }
            std::string record_id;
            Field field;
            if (!unescape(columns[1], record_id) ||
                !unescape(columns[2], field.id) ||
                !unescape(columns[3], field.name) ||
                !unescape(columns[4], field.canonical_type) ||
                !parseInteger(columns[5], field.offset_bits) ||
                !parseInteger(columns[6], field.size_bytes) ||
                !parseInteger(columns[7], field.alignment_bytes) ||
                !parseInteger(columns[8], field.bit_width) ||
                !unescape(columns[9], field.location.file) ||
                !parseInteger(columns[10], field.location.line) ||
                !parseInteger(columns[11], field.location.column)) {
                error = "invalid semantic field values at line " +
                    std::to_string(line_number);
                return false;
            }
            const auto record = record_positions.find(record_id);
            if (record == record_positions.end()) {
                error = "semantic field references an unknown record at line " +
                    std::to_string(line_number);
                return false;
            }
            index.records[record->second].fields.push_back(std::move(field));
            continue;
        }
        error = "unknown semantic index row at line " +
            std::to_string(line_number);
        return false;
    }
    if (!input.eof()) {
        error = "could not read semantic index '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

DiffSummary diff(const Index &old_index, const Index &new_index)
{
    DiffSummary summary;
    std::ostringstream output;
    std::set<std::string> matched_new_records;

    for (const Record &old_record : old_index.records) {
        const Record *new_record = findRecord(new_index, old_record);
        if (new_record == nullptr) {
            summary.changed = true;
            summary.incompatible = true;
            ++summary.changed_records;
            output << "record removed: " << old_record.kind << ' '
                   << old_record.name << '\n';
            continue;
        }
        matched_new_records.insert(new_record->id);
        bool changed = old_record.size_bytes != new_record->size_bytes ||
            old_record.alignment_bytes != new_record->alignment_bytes;
        bool append_only = true;
        std::ostringstream details;

        for (const Field &old_field : old_record.fields) {
            const Field *new_field = findField(*new_record, old_field);
            if (new_field == nullptr) {
                changed = true;
                append_only = false;
                details << "  - field " << old_field.name << ": "
                        << fieldDescription(old_field) << '\n';
            } else if (!sameField(old_field, *new_field)) {
                changed = true;
                append_only = false;
                details << "  ~ field " << old_field.name << ": "
                        << fieldDescription(old_field) << " -> "
                        << fieldDescription(*new_field) << '\n';
            }
        }
        for (const Field &new_field : new_record->fields) {
            if (findField(old_record, new_field) != nullptr) {
                continue;
            }
            changed = true;
            const bool appended =
                new_field.offset_bits >= old_record.size_bytes * 8;
            append_only = append_only && appended;
            details << "  + field " << new_field.name << ": "
                    << fieldDescription(new_field);
            if (!appended) {
                details << " (inserted into existing layout)";
            }
            details << '\n';
        }
        if (!changed) {
            continue;
        }

        summary.changed = true;
        ++summary.changed_records;
        append_only = append_only &&
            new_record->size_bytes >= old_record.size_bytes &&
            new_record->alignment_bytes == old_record.alignment_bytes;
        output << old_record.kind << ' ' << old_record.name << ":\n";
        if (old_record.size_bytes != new_record->size_bytes) {
            output << "  size: " << old_record.size_bytes << " -> "
                   << new_record->size_bytes << " bytes\n";
        }
        if (old_record.alignment_bytes != new_record->alignment_bytes) {
            output << "  alignment: " << old_record.alignment_bytes << " -> "
                   << new_record->alignment_bytes << " bytes\n";
        }
        output << details.str();
        if (append_only) {
            ++summary.append_only_records;
            output << "  classification: append-only layout candidate; "
                      "live storage migration is still required\n";
        } else {
            summary.incompatible = true;
            output << "  classification: incompatible native layout\n";
        }
    }

    for (const Record &new_record : new_index.records) {
        if (matched_new_records.count(new_record.id) != 0U) {
            continue;
        }
        bool matched_by_name = false;
        for (const Record &old_record : old_index.records) {
            matched_by_name = matched_by_name ||
                (old_record.kind == new_record.kind &&
                 old_record.name == new_record.name);
        }
        if (!matched_by_name) {
            summary.changed = true;
            ++summary.changed_records;
            output << "record added: " << new_record.kind << ' '
                   << new_record.name << " (" << new_record.size_bytes
                   << " bytes, alignment " << new_record.alignment_bytes
                   << ")\n";
        }
    }

    if (!summary.changed) {
        summary.text = "no semantic record-layout changes\n";
        return summary;
    }
    std::ostringstream header;
    header << "CVite semantic record-layout diff: "
           << summary.changed_records << " changed record(s)";
    if (summary.append_only_records != 0U) {
        header << ", " << summary.append_only_records
               << " append-only candidate(s)";
    }
    header << '\n';
    summary.text = header.str() + output.str();
    return summary;
}

} // namespace cvite::semantic
