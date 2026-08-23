struct FieldManifest final {
    std::string id;
    std::string name;
    std::string type;
    std::string canonical_type;
    long long offset_bits = -1;
    long long size_bytes = -1;
    bool bitfield = false;
    int bit_width = -1;
    bool flexible_array = false;
};

struct RecordManifest final {
    std::string id;
    std::string identity;
    std::string usr;
    std::string name;
    std::string kind;
    SourceLocation location;
    long long size_bytes = -1;
    long long alignment_bytes = -1;
    std::string layout_fingerprint;
    std::vector<std::string> aliases;
    std::vector<FieldManifest> fields;
};

struct FunctionManifest final {
    std::string id;
    std::string identity;
    std::string usr;
    std::string name;
    std::string display_name;
    std::string mangling;
    std::string type;
    std::string canonical_type;
    std::string linkage;
    std::string storage_class;
    SourceLocation location;
    bool variadic = false;
    unsigned calling_convention = 0U;
    std::string semantic_fingerprint;
};

struct GlobalManifest final {
    std::string id;
    std::string identity;
    std::string usr;
    std::string name;
    std::string type;
    std::string canonical_type;
    std::string linkage;
    std::string storage_class;
    SourceLocation location;
    long long size_bytes = -1;
    long long alignment_bytes = -1;
    bool static_local = false;
    std::string layout_fingerprint;
};

struct TranslationUnitManifest final {
    std::string source;
    std::string target_triple;
    int pointer_width = 0;
    std::vector<std::string> dependencies;
    std::vector<FunctionManifest> functions;
    std::vector<RecordManifest> records;
    std::vector<GlobalManifest> globals;
};

struct CollectionContext final {
    const Options *options = nullptr;
    TranslationUnitManifest *manifest = nullptr;
    std::map<std::string, std::vector<std::string>> aliases_by_record_usr;
    std::set<std::string> seen_function_ids;
    std::set<std::string> seen_record_ids;
    std::set<std::string> seen_global_ids;
};

[[nodiscard]] bool cursor_is_project_declaration(
    CXCursor cursor,
    const Options &options)
{
    const CXSourceLocation location = clang_getCursorLocation(cursor);
    CXFile file = nullptr;
    unsigned line = 0U;
    unsigned column = 0U;
    unsigned offset = 0U;

    if (clang_equalLocations(location, clang_getNullLocation()) != 0 ||
        clang_Location_isInSystemHeader(location) != 0) {
        return false;
    }

    clang_getExpansionLocation(location, &file, &line, &column, &offset);
    (void)line;
    (void)column;
    (void)offset;
    if (file == nullptr) {
        return false;
    }

    const std::string filename = clang_string(clang_getFileName(file));
    std::error_code path_error;
    const fs::path absolute = fs::absolute(filename, path_error).lexically_normal();
    return !path_error && path_is_within(absolute, options.project_root);
}

struct FieldVisitContext final {
    const Options *options = nullptr;
    std::vector<FieldManifest> *fields = nullptr;
};

CXChildVisitResult collect_field(
    CXCursor cursor,
    CXCursor parent,
    CXClientData client_data)
{
    (void)parent;
    auto *context = static_cast<FieldVisitContext *>(client_data);
    if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
        return CXChildVisit_Continue;
    }

    FieldManifest field;
    const SourceLocation location =
        cursor_location(cursor, context->options->project_root);
    const CXType field_type = clang_getCursorType(cursor);
    const CXType canonical_type = clang_getCanonicalType(field_type);

    field.id = entity_id(*context->options, "field", cursor, location);
    field.name = cursor_spelling(cursor);
    field.type = type_spelling(field_type);
    field.canonical_type = canonical_type_spelling(field_type);
    field.offset_bits = clang_Cursor_getOffsetOfField(cursor);
    field.size_bytes = clang_Type_getSizeOf(canonical_type);
    field.bitfield = clang_Cursor_isBitField(cursor) != 0;
    if (field.bitfield) {
        field.bit_width = clang_getFieldDeclBitWidth(cursor);
    }
    field.flexible_array = canonical_type.kind == CXType_IncompleteArray;
    context->fields->push_back(std::move(field));
    return CXChildVisit_Continue;
}

[[nodiscard]] std::string record_layout_seed(const RecordManifest &record)
{
    std::ostringstream seed;
    seed << kFingerprintSchema << "\nrecord\n" << record.kind << '\n'
         << record.size_bytes << '\n' << record.alignment_bytes;
    for (const FieldManifest &field : record.fields) {
        seed << "\nfield\n" << field.name << '\n' << field.canonical_type << '\n'
             << field.offset_bits << '\n' << field.size_bytes << '\n'
             << field.bitfield << '\n' << field.bit_width << '\n'
             << field.flexible_array;
    }
    return seed.str();
}

void collect_record(CXCursor cursor, CollectionContext &context)
{
    const SourceLocation location =
        cursor_location(cursor, context.options->project_root);
    RecordManifest record;
    const CXType record_type = clang_getCursorType(cursor);
    const CXCursorKind kind = clang_getCursorKind(cursor);
    FieldVisitContext field_context;

    record.id = entity_id(*context.options, "record", cursor, location);
    if (!context.seen_record_ids.insert(record.id).second) {
        return;
    }

    record.identity = entity_identity(*context.options, "record", cursor, location);
    record.usr = cursor_usr(cursor);
    record.name = cursor_spelling(cursor);
    record.kind = kind == CXCursor_UnionDecl ? "union" : "struct";
    record.location = location;
    record.size_bytes = clang_Type_getSizeOf(record_type);
    record.alignment_bytes = clang_Type_getAlignOf(record_type);

    field_context.options = context.options;
    field_context.fields = &record.fields;
    clang_visitChildren(cursor, collect_field, &field_context);

    record.layout_fingerprint = hash128(record_layout_seed(record)).hex();
    context.manifest->records.push_back(std::move(record));
}

[[nodiscard]] std::string function_semantic_seed(
    const FunctionManifest &function)
{
    std::ostringstream seed;
    seed << kFingerprintSchema << "\nfunction\n"
         << function.canonical_type << '\n'
         << function.calling_convention << '\n'
         << function.variadic << '\n'
         << function.linkage << '\n'
         << function.storage_class;
    return seed.str();
}

void collect_function(CXCursor cursor, CollectionContext &context)
{
    const SourceLocation location =
        cursor_location(cursor, context.options->project_root);
    FunctionManifest function;
    const CXType function_type = clang_getCursorType(cursor);
    const CXType canonical_type = clang_getCanonicalType(function_type);

    function.id = entity_id(*context.options, "function", cursor, location);
    if (!context.seen_function_ids.insert(function.id).second) {
        return;
    }

    function.identity =
        entity_identity(*context.options, "function", cursor, location);
    function.usr = cursor_usr(cursor);
    function.name = cursor_spelling(cursor);
    function.display_name = cursor_display_name(cursor);
    function.mangling = clang_string(clang_Cursor_getMangling(cursor));
    function.type = type_spelling(function_type);
    function.canonical_type = canonical_type_spelling(function_type);
    function.linkage = linkage_name(clang_getCursorLinkage(cursor));
    function.storage_class =
        storage_class_name(clang_Cursor_getStorageClass(cursor));
    function.location = location;
    function.variadic = clang_isFunctionTypeVariadic(canonical_type) != 0;
    function.calling_convention = static_cast<unsigned>(
        clang_getFunctionTypeCallingConv(canonical_type));
    function.semantic_fingerprint = hash128(function_semantic_seed(function)).hex();
    context.manifest->functions.push_back(std::move(function));
}

[[nodiscard]] bool cursor_is_definition(CXCursor cursor)
{
    const CXCursor definition = clang_getCursorDefinition(cursor);
    return clang_Cursor_isNull(definition) == 0 &&
        clang_equalCursors(cursor, definition) != 0;
}

[[nodiscard]] bool cursor_is_static_local(CXCursor cursor)
{
    const CXCursor semantic_parent = clang_getCursorSemanticParent(cursor);
    return clang_getCursorKind(semantic_parent) == CXCursor_FunctionDecl &&
        clang_Cursor_getStorageClass(cursor) == CX_SC_Static;
}

[[nodiscard]] bool cursor_is_file_scope_variable(CXCursor cursor)
{
    const CXCursor semantic_parent = clang_getCursorSemanticParent(cursor);
    return clang_getCursorKind(semantic_parent) == CXCursor_TranslationUnit;
}

[[nodiscard]] std::string global_layout_seed(const GlobalManifest &global)
{
    std::ostringstream seed;
    seed << kFingerprintSchema << "\nstorage\n"
         << global.canonical_type << '\n'
         << global.size_bytes << '\n'
         << global.alignment_bytes;
    return seed.str();
}

void collect_global(CXCursor cursor, CollectionContext &context)
{
    const bool static_local = cursor_is_static_local(cursor);
    const bool file_scope = cursor_is_file_scope_variable(cursor);
    if (!static_local && !file_scope) {
        return;
    }

    const CX_StorageClass storage_class = clang_Cursor_getStorageClass(cursor);
    const bool tentative_definition =
        file_scope && storage_class != CX_SC_Extern;
    if (!cursor_is_definition(cursor) && !tentative_definition) {
        return;
    }

    const SourceLocation location =
        cursor_location(cursor, context.options->project_root);
    GlobalManifest global;
    const CXType type = clang_getCursorType(cursor);
    const CXType canonical_type = clang_getCanonicalType(type);

    global.id = entity_id(*context.options, "storage", cursor, location);
    if (!context.seen_global_ids.insert(global.id).second) {
        return;
    }

    global.identity = entity_identity(*context.options, "storage", cursor, location);
    global.usr = cursor_usr(cursor);
    global.name = cursor_spelling(cursor);
    global.type = type_spelling(type);
    global.canonical_type = canonical_type_spelling(type);
    global.linkage = linkage_name(clang_getCursorLinkage(cursor));
    global.storage_class = storage_class_name(clang_Cursor_getStorageClass(cursor));
    global.location = location;
    global.size_bytes = clang_Type_getSizeOf(canonical_type);
    global.alignment_bytes = clang_Type_getAlignOf(canonical_type);
    global.static_local = static_local;
    global.layout_fingerprint = hash128(global_layout_seed(global)).hex();
    context.manifest->globals.push_back(std::move(global));
}

void collect_typedef(CXCursor cursor, CollectionContext &context)
{
    const CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
    const CXCursor declaration = clang_getTypeDeclaration(underlying);
    const CXCursorKind kind = clang_getCursorKind(declaration);
    if (kind != CXCursor_StructDecl && kind != CXCursor_UnionDecl) {
        return;
    }

    const std::string usr = cursor_usr(declaration);
    const std::string alias = cursor_spelling(cursor);
    if (!usr.empty() && !alias.empty()) {
        context.aliases_by_record_usr[usr].push_back(alias);
    }
}

CXChildVisitResult collect_cursor(
    CXCursor cursor,
    CXCursor parent,
    CXClientData client_data)
{
    (void)parent;
    auto *context = static_cast<CollectionContext *>(client_data);
    const CXCursorKind kind = clang_getCursorKind(cursor);

    if (!cursor_is_project_declaration(cursor, *context->options)) {
        return CXChildVisit_Continue;
    }

    switch (kind) {
    case CXCursor_FunctionDecl:
        if (cursor_is_definition(cursor)) {
            collect_function(cursor, *context);
        }
        break;
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
        if (cursor_is_definition(cursor)) {
            collect_record(cursor, *context);
        }
        break;
    case CXCursor_VarDecl:
        collect_global(cursor, *context);
        break;
    case CXCursor_TypedefDecl:
        collect_typedef(cursor, *context);
        break;
    default:
        break;
    }

    return CXChildVisit_Recurse;
}

struct InclusionContext final {
    const Options *options = nullptr;
    std::set<std::string> *dependencies = nullptr;
};

void collect_inclusion(
    CXFile included_file,
    CXSourceLocation *inclusion_stack,
    unsigned include_len,
    CXClientData client_data)
{
    (void)inclusion_stack;
    (void)include_len;
    auto *context = static_cast<InclusionContext *>(client_data);
    const std::string filename = clang_string(clang_getFileName(included_file));
    if (filename.empty()) {
        return;
    }

    std::error_code error;
    const fs::path absolute = fs::absolute(filename, error).lexically_normal();
    if (!error && path_is_within(absolute, context->options->project_root)) {
        context->dependencies->insert(normalize_manifest_path(
            absolute, context->options->project_root));
    }
}

void apply_record_aliases(CollectionContext &context)
{
    for (RecordManifest &record : context.manifest->records) {
        const auto aliases = context.aliases_by_record_usr.find(record.usr);
        if (aliases != context.aliases_by_record_usr.end()) {
            record.aliases = aliases->second;
            std::sort(record.aliases.begin(), record.aliases.end());
            record.aliases.erase(
                std::unique(record.aliases.begin(), record.aliases.end()),
                record.aliases.end());
        }
        if (record.name.empty()) {
            if (!record.aliases.empty()) {
                record.name = record.aliases.front();
            } else {
                std::ostringstream anonymous;
                anonymous << "<anonymous@" << record.location.path << ':'
                          << record.location.line << ':' << record.location.column
                          << '>';
                record.name = anonymous.str();
            }
        }
    }
}

template <typename Entity>
void sort_by_id(std::vector<Entity> &entities)
{
    std::sort(
        entities.begin(),
        entities.end(),
        [](const Entity &left, const Entity &right) { return left.id < right.id; });
}

[[nodiscard]] bool print_diagnostics(CXTranslationUnit unit)
{
    bool has_error = false;
    const unsigned count = clang_getNumDiagnostics(unit);

    for (unsigned index = 0U; index < count; ++index) {
        CXDiagnostic diagnostic = clang_getDiagnostic(unit, index);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        const unsigned flags = clang_defaultDiagnosticDisplayOptions();
        std::cerr << clang_string(clang_formatDiagnostic(diagnostic, flags)) << '\n';
        if (severity == CXDiagnostic_Error || severity == CXDiagnostic_Fatal) {
            has_error = true;
        }
        clang_disposeDiagnostic(diagnostic);
    }

    return has_error;
}

[[nodiscard]] bool build_manifest(
    const Options &options,
    TranslationUnitManifest &manifest,
    std::string &compiler_version,
    std::string &error)
{
    CXIndex index = clang_createIndex(1, 0);
    if (index == nullptr) {
        error = "libclang could not create an index";
        return false;
    }

    std::vector<const char *> arguments;
    arguments.reserve(options.clang_arguments.size());
    for (const std::string &argument : options.clang_arguments) {
        arguments.push_back(argument.c_str());
    }

    CXTranslationUnit unit = nullptr;
    const unsigned flags =
        static_cast<unsigned>(CXTranslationUnit_DetailedPreprocessingRecord) |
        static_cast<unsigned>(CXTranslationUnit_KeepGoing);
    const CXErrorCode parse_status = clang_parseTranslationUnit2(
        index,
        options.source.string().c_str(),
        arguments.data(),
        static_cast<int>(arguments.size()),
        nullptr,
        0U,
        flags,
        &unit);

    if (parse_status != CXError_Success || unit == nullptr) {
        std::ostringstream message;
        message << "libclang failed to parse " << options.source
                << " (error " << static_cast<int>(parse_status) << ')';
        error = message.str();
        clang_disposeIndex(index);
        return false;
    }

    compiler_version = clang_string(clang_getClangVersion());
    if (print_diagnostics(unit)) {
        error = "candidate translation unit contains compiler errors";
        clang_disposeTranslationUnit(unit);
        clang_disposeIndex(index);
        return false;
    }

    manifest.source = normalize_manifest_path(options.source, options.project_root);
    CXTargetInfo target_info = clang_getTranslationUnitTargetInfo(unit);
    if (target_info != nullptr) {
        manifest.target_triple = clang_string(clang_TargetInfo_getTriple(target_info));
        manifest.pointer_width = clang_TargetInfo_getPointerWidth(target_info);
        clang_TargetInfo_dispose(target_info);
    }

    std::set<std::string> dependencies;
    InclusionContext inclusion_context{&options, &dependencies};
    clang_getInclusions(unit, collect_inclusion, &inclusion_context);
    dependencies.insert(manifest.source);
    manifest.dependencies.assign(dependencies.begin(), dependencies.end());

    CollectionContext collection;
    collection.options = &options;
    collection.manifest = &manifest;
    clang_visitChildren(
        clang_getTranslationUnitCursor(unit), collect_cursor, &collection);
    apply_record_aliases(collection);

    sort_by_id(manifest.functions);
    sort_by_id(manifest.records);
    sort_by_id(manifest.globals);

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    return true;
}
