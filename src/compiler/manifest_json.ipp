void write_location(std::ostream &output, const SourceLocation &location, unsigned level)
{
    output << "{\n";
    indent(output, level + 1U);
    output << "\"path\": ";
    write_json_string(output, location.path);
    output << ",\n";
    indent(output, level + 1U);
    output << "\"line\": " << location.line << ",\n";
    indent(output, level + 1U);
    output << "\"column\": " << location.column << '\n';
    indent(output, level);
    output << '}';
}

void write_string_array(
    std::ostream &output,
    const std::vector<std::string> &values,
    unsigned level)
{
    output << '[';
    if (!values.empty()) {
        output << '\n';
        for (std::size_t index = 0U; index < values.size(); ++index) {
            indent(output, level + 1U);
            write_json_string(output, values[index]);
            output << (index + 1U == values.size() ? "\n" : ",\n");
        }
        indent(output, level);
    }
    output << ']';
}

void write_field(std::ostream &output, const FieldManifest &field, unsigned level)
{
    output << "{\n";
#define CVITE_JSON_FIELD_STRING(NAME, VALUE, LAST)                              \
    do {                                                                        \
        indent(output, level + 1U);                                             \
        output << "\"" NAME "\": ";                                      \
        write_json_string(output, VALUE);                                       \
        output << (LAST ? "\n" : ",\n");                                  \
    } while (false)
#define CVITE_JSON_FIELD_VALUE(NAME, VALUE, LAST)                               \
    do {                                                                        \
        indent(output, level + 1U);                                             \
        output << "\"" NAME "\": " << VALUE                              \
               << (LAST ? "\n" : ",\n");                                  \
    } while (false)
    CVITE_JSON_FIELD_STRING("id", field.id, false);
    CVITE_JSON_FIELD_STRING("name", field.name, false);
    CVITE_JSON_FIELD_STRING("type", field.type, false);
    CVITE_JSON_FIELD_STRING("canonical_type", field.canonical_type, false);
    CVITE_JSON_FIELD_VALUE("offset_bits", field.offset_bits, false);
    CVITE_JSON_FIELD_VALUE("size_bytes", field.size_bytes, false);
    CVITE_JSON_FIELD_VALUE("bitfield", (field.bitfield ? "true" : "false"), false);
    CVITE_JSON_FIELD_VALUE("bit_width", field.bit_width, false);
    CVITE_JSON_FIELD_VALUE(
        "flexible_array", (field.flexible_array ? "true" : "false"), true);
#undef CVITE_JSON_FIELD_STRING
#undef CVITE_JSON_FIELD_VALUE
    indent(output, level);
    output << '}';
}

void write_function(
    std::ostream &output,
    const FunctionManifest &function,
    unsigned level)
{
    output << "{\n";
    const auto string_field = [&](std::string_view name, std::string_view value) {
        indent(output, level + 1U);
        write_json_string(output, name);
        output << ": ";
        write_json_string(output, value);
        output << ",\n";
    };
    string_field("id", function.id);
    string_field("identity", function.identity);
    string_field("usr", function.usr);
    string_field("name", function.name);
    string_field("display_name", function.display_name);
    string_field("mangling", function.mangling);
    string_field("type", function.type);
    string_field("canonical_type", function.canonical_type);
    string_field("linkage", function.linkage);
    string_field("storage_class", function.storage_class);
    string_field("semantic_fingerprint", function.semantic_fingerprint);
    indent(output, level + 1U);
    output << "\"variadic\": " << (function.variadic ? "true" : "false")
           << ",\n";
    indent(output, level + 1U);
    output << "\"calling_convention\": " << function.calling_convention << ",\n";
    indent(output, level + 1U);
    output << "\"location\": ";
    write_location(output, function.location, level + 1U);
    output << '\n';
    indent(output, level);
    output << '}';
}

void write_record(std::ostream &output, const RecordManifest &record, unsigned level)
{
    output << "{\n";
    const auto string_field = [&](std::string_view name, std::string_view value) {
        indent(output, level + 1U);
        write_json_string(output, name);
        output << ": ";
        write_json_string(output, value);
        output << ",\n";
    };
    string_field("id", record.id);
    string_field("identity", record.identity);
    string_field("usr", record.usr);
    string_field("name", record.name);
    string_field("kind", record.kind);
    string_field("layout_fingerprint", record.layout_fingerprint);
    indent(output, level + 1U);
    output << "\"size_bytes\": " << record.size_bytes << ",\n";
    indent(output, level + 1U);
    output << "\"alignment_bytes\": " << record.alignment_bytes << ",\n";
    indent(output, level + 1U);
    output << "\"aliases\": ";
    write_string_array(output, record.aliases, level + 1U);
    output << ",\n";
    indent(output, level + 1U);
    output << "\"fields\": [";
    if (!record.fields.empty()) {
        output << '\n';
        for (std::size_t index = 0U; index < record.fields.size(); ++index) {
            indent(output, level + 2U);
            write_field(output, record.fields[index], level + 2U);
            output << (index + 1U == record.fields.size() ? "\n" : ",\n");
        }
        indent(output, level + 1U);
    }
    output << "],\n";
    indent(output, level + 1U);
    output << "\"location\": ";
    write_location(output, record.location, level + 1U);
    output << '\n';
    indent(output, level);
    output << '}';
}

void write_global(std::ostream &output, const GlobalManifest &global, unsigned level)
{
    output << "{\n";
    const auto string_field = [&](std::string_view name, std::string_view value) {
        indent(output, level + 1U);
        write_json_string(output, name);
        output << ": ";
        write_json_string(output, value);
        output << ",\n";
    };
    string_field("id", global.id);
    string_field("identity", global.identity);
    string_field("usr", global.usr);
    string_field("name", global.name);
    string_field("type", global.type);
    string_field("canonical_type", global.canonical_type);
    string_field("linkage", global.linkage);
    string_field("storage_class", global.storage_class);
    string_field("layout_fingerprint", global.layout_fingerprint);
    indent(output, level + 1U);
    output << "\"size_bytes\": " << global.size_bytes << ",\n";
    indent(output, level + 1U);
    output << "\"alignment_bytes\": " << global.alignment_bytes << ",\n";
    indent(output, level + 1U);
    output << "\"static_local\": " << (global.static_local ? "true" : "false")
           << ",\n";
    indent(output, level + 1U);
    output << "\"location\": ";
    write_location(output, global.location, level + 1U);
    output << '\n';
    indent(output, level);
    output << '}';
}

template <typename Entity, typename Writer>
void write_entity_array(
    std::ostream &output,
    const std::vector<Entity> &entities,
    unsigned level,
    Writer writer)
{
    output << '[';
    if (!entities.empty()) {
        output << '\n';
        for (std::size_t index = 0U; index < entities.size(); ++index) {
            indent(output, level + 1U);
            writer(output, entities[index], level + 1U);
            output << (index + 1U == entities.size() ? "\n" : ",\n");
        }
        indent(output, level);
    }
    output << ']';
}

void write_manifest(
    std::ostream &output,
    const Options &options,
    const TranslationUnitManifest &manifest,
    std::string_view compiler_version)
{
    output << "{\n";
    indent(output, 1U);
    output << "\"schema\": " << kManifestSchema << ",\n";
    indent(output, 1U);
    output << "\"tool\": \"cvite-manifest\",\n";
    indent(output, 1U);
    output << "\"compiler\": ";
    write_json_string(output, compiler_version);
    output << ",\n";
    indent(output, 1U);
    output << "\"project_id\": ";
    write_json_string(output, options.project_id);
    output << ",\n";
    indent(output, 1U);
    output << "\"source\": ";
    write_json_string(output, manifest.source);
    output << ",\n";
    indent(output, 1U);
    output << "\"target_triple\": ";
    write_json_string(output, manifest.target_triple);
    output << ",\n";
    indent(output, 1U);
    output << "\"pointer_width\": " << manifest.pointer_width << ",\n";
    indent(output, 1U);
    output << "\"dependencies\": ";
    write_string_array(output, manifest.dependencies, 1U);
    output << ",\n";
    indent(output, 1U);
    output << "\"functions\": ";
    write_entity_array(output, manifest.functions, 1U, write_function);
    output << ",\n";
    indent(output, 1U);
    output << "\"records\": ";
    write_entity_array(output, manifest.records, 1U, write_record);
    output << ",\n";
    indent(output, 1U);
    output << "\"globals\": ";
    write_entity_array(output, manifest.globals, 1U, write_global);
    output << '\n' << "}\n";
}
