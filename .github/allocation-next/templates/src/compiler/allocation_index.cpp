#include "cvite/allocation_index.hpp"

#include <clang-c/Index.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
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

bool isRecordCursor(CXCursor cursor)
{
    const CXCursorKind kind = clang_getCursorKind(cursor);
    return kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl;
}

struct TypeIdentity final {
    std::string id;
    std::string name;
};

TypeIdentity identityForType(CXType input)
{
    if (input.kind == CXType_Pointer || input.kind == CXType_BlockPointer) {
        input = clang_getPointeeType(input);
    }
    const CXType canonical = clang_getCanonicalType(input);
    CXCursor declaration = clang_getTypeDeclaration(canonical);
    if (!isRecordCursor(declaration)) {
        return {};
    }
    TypeIdentity result;
    result.id = takeString(clang_getCursorUSR(declaration));
    result.name = takeString(clang_getCursorSpelling(declaration));
    if (result.name.empty()) {
        result.name = takeString(clang_getTypeSpelling(canonical));
    }
    if (result.id.empty()) {
        const Location location = cursorLocation(declaration);
        std::ostringstream output;
        output << location.path << ':' << location.line << ':' << location.column
               << ':' << result.name;
        result.id = output.str();
    }
    return result;
}

TypeIdentity identityForCursorType(CXCursor cursor)
{
    return identityForType(clang_getCursorType(cursor));
}

struct TypeReferenceContext final {
    std::map<std::string, TypeIdentity> identities;
};

CXChildVisitResult collectTypeReferences(
    CXCursor cursor,
    CXCursor,
    CXClientData client_data)
{
    auto &context = *static_cast<TypeReferenceContext *>(client_data);
    if (clang_getCursorKind(cursor) == CXCursor_TypeRef) {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        TypeIdentity identity = identityForCursorType(referenced);
        if (!identity.id.empty()) {
            context.identities[identity.id] = std::move(identity);
        }
    }
    return CXChildVisit_Recurse;
}

std::string calleeName(CXCursor call)
{
    CXCursor referenced = clang_getCursorReferenced(call);
    std::string name = takeString(clang_getCursorSpelling(referenced));
    if (!name.empty()) {
        return name;
    }

    struct Context final {
        std::string name;
    } context;
    clang_visitChildren(
        call,
        [](CXCursor child, CXCursor, CXClientData data) {
            auto &state = *static_cast<Context *>(data);
            if (state.name.empty() &&
                clang_getCursorKind(child) == CXCursor_DeclRefExpr) {
                state.name = takeString(clang_getCursorSpelling(child));
            }
            return state.name.empty() ? CXChildVisit_Recurse
                                      : CXChildVisit_Break;
        },
        &context);
    return context.name;
}

bool classifyAllocator(
    const std::string &name,
    AllocationKind &kind)
{
    if (name == "malloc") {
        kind = AllocationKind::malloc_call;
        return true;
    }
    if (name == "calloc") {
        kind = AllocationKind::calloc_call;
        return true;
    }
    if (name == "realloc") {
        kind = AllocationKind::realloc_call;
        return true;
    }
    if (name == "aligned_alloc") {
        kind = AllocationKind::aligned_alloc_call;
        return true;
    }
    return false;
}

std::string siteId(
    const Location &location,
    const std::string &callee)
{
    std::ostringstream output;
    output << location.path << ':' << location.line << ':' << location.column
           << ':' << callee;
    return output.str();
}

struct AllocationContext final {
    std::map<std::string, AllocationSite> sites;
};

CXChildVisitResult visitAllocation(
    CXCursor cursor,
    CXCursor parent,
    CXClientData client_data)
{
    if (clang_getCursorKind(cursor) != CXCursor_CallExpr) {
        return CXChildVisit_Recurse;
    }

    const std::string callee = calleeName(cursor);
    AllocationKind kind = AllocationKind::malloc_call;
    if (!classifyAllocator(callee, kind)) {
        return CXChildVisit_Recurse;
    }

    AllocationSite site;
    const Location location = cursorLocation(cursor);
    site.id = siteId(location, callee);
    site.source = location.path;
    site.line = location.line;
    site.column = location.column;
    site.kind = kind;

    TypeIdentity result_type = identityForCursorType(parent);
    if (!result_type.id.empty()) {
        site.confidence = AllocationConfidence::typed_result;
        site.type_id = std::move(result_type.id);
        site.type_name = std::move(result_type.name);
    } else {
        TypeReferenceContext references;
        clang_visitChildren(cursor, collectTypeReferences, &references);
        if (references.identities.size() == 1U) {
            const TypeIdentity &identity = references.identities.begin()->second;
            site.confidence = AllocationConfidence::sizeof_type;
            site.type_id = identity.id;
            site.type_name = identity.name;
        }
    }

    auto &context = *static_cast<AllocationContext *>(client_data);
    context.sites[site.id] = std::move(site);
    return CXChildVisit_Recurse;
}

std::string diagnosticText(CXDiagnostic diagnostic)
{
    return takeString(clang_formatDiagnostic(
        diagnostic,
        clang_defaultDiagnosticDisplayOptions()));
}

std::string encode(const std::string &text)
{
    if (text.empty()) {
        return "-";
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (char character : text) {
        output << std::setw(2)
               << static_cast<unsigned>(
                      static_cast<unsigned char>(character));
    }
    return output.str();
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

bool decode(const std::string &text, std::string &output)
{
    output.clear();
    if (text == "-") {
        return true;
    }
    if ((text.size() % 2U) != 0U) {
        return false;
    }
    output.reserve(text.size() / 2U);
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        const int high = hexValue(text[index]);
        const int low = hexValue(text[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::vector<std::string> split(const std::string &line)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        result.push_back(line.substr(
            start,
            tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) {
            return result;
        }
        start = tab + 1U;
    }
}

} // namespace

bool buildAllocationIndex(
    const std::string &source,
    const std::vector<std::string> &arguments,
    AllocationIndex &index,
    std::string &diagnostics)
{
    index = AllocationIndex{};
    diagnostics.clear();
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
    for (unsigned value = 0; value < diagnostic_count; ++value) {
        CXDiagnostic diagnostic = clang_getDiagnostic(unit, value);
        if (!diagnostics.empty()) {
            diagnostics.push_back('\n');
        }
        diagnostics += diagnosticText(diagnostic);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        has_error = has_error || severity == CXDiagnostic_Error ||
            severity == CXDiagnostic_Fatal;
        clang_disposeDiagnostic(diagnostic);
    }

    CXTargetInfo target = clang_getTranslationUnitTargetInfo(unit);
    if (target != nullptr) {
        index.target = takeString(clang_TargetInfo_getTriple(target));
        clang_TargetInfo_dispose(target);
    }

    AllocationContext context;
    clang_visitChildren(
        clang_getTranslationUnitCursor(unit), visitAllocation, &context);
    index.sites.reserve(context.sites.size());
    for (auto &entry : context.sites) {
        index.sites.push_back(std::move(entry.second));
    }

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(clang_index);
    return !has_error;
}

bool writeAllocationIndex(
    const AllocationIndex &index,
    const std::string &path,
    std::string &error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "cannot open allocation index for writing: " + path;
        return false;
    }
    output << "CVITE_ALLOCATION_INDEX_V1\n";
    output << "TARGET\t" << encode(index.target) << '\n';
    for (const AllocationSite &site : index.sites) {
        output << "SITE\t" << encode(site.id) << '\t' << encode(site.source)
               << '\t' << site.line << '\t' << site.column << '\t'
               << static_cast<unsigned>(site.kind) << '\t'
               << static_cast<unsigned>(site.confidence) << '\t'
               << encode(site.type_id) << '\t' << encode(site.type_name)
               << '\n';
    }
    if (!output) {
        error = "failed while writing allocation index: " + path;
        return false;
    }
    error.clear();
    return true;
}

bool readAllocationIndex(
    const std::string &path,
    AllocationIndex &index,
    std::string &error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open allocation index: " + path;
        return false;
    }
    index = AllocationIndex{};
    std::string line;
    if (!std::getline(input, line) || line != "CVITE_ALLOCATION_INDEX_V1") {
        error = "unsupported allocation index schema";
        return false;
    }
    if (!std::getline(input, line)) {
        error = "allocation index has no target";
        return false;
    }
    auto target = split(line);
    if (target.size() != 2U || target[0] != "TARGET" ||
        !decode(target[1], index.target)) {
        error = "invalid allocation target record";
        return false;
    }
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto values = split(line);
        if (values.size() != 9U || values[0] != "SITE") {
            error = "invalid allocation site record";
            return false;
        }
        AllocationSite site;
        try {
            if (!decode(values[1], site.id) ||
                !decode(values[2], site.source) ||
                !decode(values[7], site.type_id) ||
                !decode(values[8], site.type_name)) {
                error = "invalid encoded allocation site text";
                return false;
            }
            site.line = static_cast<std::uint32_t>(std::stoul(values[3]));
            site.column = static_cast<std::uint32_t>(std::stoul(values[4]));
            site.kind = static_cast<AllocationKind>(std::stoul(values[5]));
            site.confidence =
                static_cast<AllocationConfidence>(std::stoul(values[6]));
        } catch (...) {
            error = "invalid allocation site number";
            return false;
        }
        index.sites.push_back(std::move(site));
    }
    error.clear();
    return true;
}

const char *allocationKindName(AllocationKind kind)
{
    switch (kind) {
    case AllocationKind::malloc_call:
        return "malloc";
    case AllocationKind::calloc_call:
        return "calloc";
    case AllocationKind::realloc_call:
        return "realloc";
    case AllocationKind::aligned_alloc_call:
        return "aligned_alloc";
    }
    return "unknown";
}

const char *allocationConfidenceName(AllocationConfidence confidence)
{
    switch (confidence) {
    case AllocationConfidence::unknown:
        return "unknown";
    case AllocationConfidence::typed_result:
        return "typed-result";
    case AllocationConfidence::sizeof_type:
        return "sizeof-type";
    }
    return "unknown";
}

std::string formatAllocationSite(const AllocationSite &site)
{
    std::ostringstream output;
    output << site.source << ':' << site.line << ':' << site.column << ' '
           << allocationKindName(site.kind) << " type="
           << (site.type_name.empty() ? "?" : site.type_name) << " confidence="
           << allocationConfidenceName(site.confidence);
    return output.str();
}

} // namespace cvite::semantic
