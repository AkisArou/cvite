constexpr std::uint32_t kManifestSchema = 1U;
constexpr std::string_view kIdentitySchema = "cvite.identity.v1";
constexpr std::string_view kFingerprintSchema = "cvite.semantic.v1";

class ClangString final {
public:
    explicit ClangString(CXString value) : value_(value) {}

    ClangString(const ClangString &) = delete;
    ClangString &operator=(const ClangString &) = delete;

    ClangString(ClangString &&other) noexcept : value_(other.value_)
    {
        other.value_.data = nullptr;
        other.value_.private_flags = 0U;
    }

    ClangString &operator=(ClangString &&other) noexcept
    {
        if (this != &other) {
            clang_disposeString(value_);
            value_ = other.value_;
            other.value_.data = nullptr;
            other.value_.private_flags = 0U;
        }
        return *this;
    }

    ~ClangString() { clang_disposeString(value_); }

    [[nodiscard]] std::string str() const
    {
        const char *text = clang_getCString(value_);
        return text != nullptr ? std::string(text) : std::string();
    }

private:
    CXString value_{};
};

[[nodiscard]] std::string clang_string(CXString value)
{
    return ClangString(value).str();
}

struct Hash128 final {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;

    [[nodiscard]] std::string hex() const
    {
        std::ostringstream output;
        output << std::hex << std::setfill('0') << std::setw(16) << high
               << std::setw(16) << low;
        return output.str();
    }
};

[[nodiscard]] std::uint64_t avalanche(std::uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value;
}

/*
 * A deterministic non-cryptographic 128-bit identifier. The complete identity
 * inputs remain in the manifest so collisions can be diagnosed instead of
 * silently accepted. This is intentionally versioned and replaceable.
 */
[[nodiscard]] Hash128 hash128(std::string_view input)
{
    std::uint64_t low = UINT64_C(14695981039346656037);
    std::uint64_t high = UINT64_C(7809847782465536322);
    std::uint64_t index = 0U;

    for (const char raw_byte : input) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        low ^= static_cast<std::uint64_t>(byte);
        low *= UINT64_C(1099511628211);

        high ^= static_cast<std::uint64_t>(byte) +
            UINT64_C(0x9e3779b97f4a7c15) + (index << 1U);
        high *= UINT64_C(14029467366897019727);
        ++index;
    }

    low ^= static_cast<std::uint64_t>(input.size());
    high ^= static_cast<std::uint64_t>(input.size()) << 32U;
    return {avalanche(high ^ low), avalanche(low ^ (high << 1U))};
}

[[nodiscard]] std::string json_escape(std::string_view input)
{
    std::ostringstream output;

    for (const char raw_character : input) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setfill('0')
                       << std::setw(4) << static_cast<unsigned>(character)
                       << std::dec;
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }

    return output.str();
}

void indent(std::ostream &output, unsigned level)
{
    for (unsigned index = 0U; index < level; ++index) {
        output << "  ";
    }
}

void write_json_string(std::ostream &output, std::string_view value)
{
    output << '"' << json_escape(value) << '"';
}

struct Options final {
    fs::path source;
    fs::path project_root;
    fs::path output;
    std::string project_id;
    std::vector<std::string> clang_arguments;
};

void print_usage(std::ostream &output)
{
    output
        << "Usage: cvite-manifest --source <file> [options] -- [clang args...]\n"
        << "\n"
        << "Options:\n"
        << "  --source <file>         translation unit to analyze\n"
        << "  --project-root <path>   root used for stable relative paths\n"
        << "  --project-id <id>       stable project identity (defaults to root path)\n"
        << "  --output <file>         output path, or '-' for stdout (default)\n"
        << "  --help                  show this help\n";
}

[[nodiscard]] bool take_option_value(
    int argc,
    char **argv,
    int &index,
    std::string &value,
    std::string &error)
{
    if (index + 1 >= argc) {
        error = std::string("missing value for ") + argv[index];
        return false;
    }
    ++index;
    value = argv[index];
    return true;
}

[[nodiscard]] std::optional<Options> parse_options(
    int argc,
    char **argv,
    std::string &error)
{
    Options options;
    bool clang_mode = false;

    options.output = "-";

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (clang_mode) {
            options.clang_arguments.push_back(argument);
            continue;
        }
        if (argument == "--") {
            clang_mode = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            return std::nullopt;
        }
        std::string value;
        if (argument == "--source") {
            if (!take_option_value(argc, argv, index, value, error)) {
                return std::nullopt;
            }
            options.source = value;
        } else if (argument == "--project-root") {
            if (!take_option_value(argc, argv, index, value, error)) {
                return std::nullopt;
            }
            options.project_root = value;
        } else if (argument == "--project-id") {
            if (!take_option_value(argc, argv, index, value, error)) {
                return std::nullopt;
            }
            options.project_id = value;
        } else if (argument == "--output" || argument == "-o") {
            if (!take_option_value(argc, argv, index, value, error)) {
                return std::nullopt;
            }
            options.output = value;
        } else {
            error = "unknown option: " + argument;
            return std::nullopt;
        }
    }

    if (options.source.empty()) {
        error = "--source is required";
        return std::nullopt;
    }

    std::error_code path_error;
    options.source = fs::absolute(options.source, path_error).lexically_normal();
    if (path_error) {
        error = "could not resolve source path: " + path_error.message();
        return std::nullopt;
    }
    if (!fs::exists(options.source)) {
        error = "source file does not exist: " + options.source.string();
        return std::nullopt;
    }

    if (options.project_root.empty()) {
        options.project_root = options.source.parent_path();
    }
    path_error.clear();
    options.project_root =
        fs::absolute(options.project_root, path_error).lexically_normal();
    if (path_error) {
        error = "could not resolve project root: " + path_error.message();
        return std::nullopt;
    }

    if (options.project_id.empty()) {
        options.project_id = options.project_root.generic_string();
    }

    return options;
}

[[nodiscard]] bool path_is_within(const fs::path &path, const fs::path &root)
{
    const fs::path relative =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }

    const auto first = relative.begin();
    return first == relative.end() || *first != fs::path("..");
}

[[nodiscard]] std::string normalize_manifest_path(
    const fs::path &path,
    const fs::path &project_root)
{
    std::error_code error;
    fs::path absolute_path = fs::absolute(path, error).lexically_normal();
    if (error) {
        absolute_path = path.lexically_normal();
    }

    if (path_is_within(absolute_path, project_root)) {
        fs::path relative = fs::relative(absolute_path, project_root, error);
        if (!error) {
            return relative.generic_string();
        }
    }
    return absolute_path.generic_string();
}

struct SourceLocation final {
    std::string path;
    unsigned line = 0U;
    unsigned column = 0U;
};

[[nodiscard]] SourceLocation cursor_location(
    CXCursor cursor,
    const fs::path &project_root)
{
    CXFile file = nullptr;
    unsigned line = 0U;
    unsigned column = 0U;
    unsigned offset = 0U;
    clang_getExpansionLocation(
        clang_getCursorLocation(cursor), &file, &line, &column, &offset);
    (void)offset;

    SourceLocation result;
    result.line = line;
    result.column = column;
    if (file != nullptr) {
        result.path = normalize_manifest_path(
            clang_string(clang_getFileName(file)), project_root);
    }
    return result;
}

[[nodiscard]] std::string cursor_usr(CXCursor cursor)
{
    return clang_string(clang_getCursorUSR(cursor));
}

[[nodiscard]] std::string cursor_spelling(CXCursor cursor)
{
    return clang_string(clang_getCursorSpelling(cursor));
}

[[nodiscard]] std::string cursor_display_name(CXCursor cursor)
{
    return clang_string(clang_getCursorDisplayName(cursor));
}

[[nodiscard]] std::string canonical_type_spelling(CXType type)
{
    return clang_string(clang_getTypeSpelling(clang_getCanonicalType(type)));
}

[[nodiscard]] std::string type_spelling(CXType type)
{
    return clang_string(clang_getTypeSpelling(type));
}

[[nodiscard]] std::string linkage_name(CXLinkageKind linkage)
{
    switch (linkage) {
    case CXLinkage_Invalid:
        return "invalid";
    case CXLinkage_NoLinkage:
        return "none";
    case CXLinkage_Internal:
        return "internal";
    case CXLinkage_UniqueExternal:
        return "unique_external";
    case CXLinkage_External:
        return "external";
    }
    return "unknown";
}

[[nodiscard]] std::string storage_class_name(CX_StorageClass storage_class)
{
    switch (storage_class) {
    case CX_SC_Invalid:
        return "invalid";
    case CX_SC_None:
        return "none";
    case CX_SC_Extern:
        return "extern";
    case CX_SC_Static:
        return "static";
    case CX_SC_PrivateExtern:
        return "private_extern";
    case CX_SC_OpenCLWorkGroupLocal:
        return "opencl_work_group_local";
    case CX_SC_Auto:
        return "auto";
    case CX_SC_Register:
        return "register";
    }
    return "unknown";
}

[[nodiscard]] std::string entity_identity(
    const Options &options,
    std::string_view kind,
    CXCursor cursor,
    const SourceLocation &location)
{
    std::ostringstream identity;
    const std::string usr = cursor_usr(cursor);
    const CXLinkageKind linkage = clang_getCursorLinkage(cursor);

    identity << kIdentitySchema << '\n'
             << options.project_id << '\n'
             << kind << '\n';
    if (!usr.empty()) {
        identity << "usr:" << usr;
    } else {
        identity << "fallback:" << location.path << ':' << location.line << ':'
                 << location.column << ':' << cursor_spelling(cursor);
    }

    if (linkage == CXLinkage_Internal || linkage == CXLinkage_NoLinkage) {
        identity << "\nfile:" << location.path;
    }
    return identity.str();
}

[[nodiscard]] std::string entity_id(
    const Options &options,
    std::string_view kind,
    CXCursor cursor,
    const SourceLocation &location)
{
    return hash128(entity_identity(options, kind, cursor, location)).hex();
}
