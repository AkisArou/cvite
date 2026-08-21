#include "run_internal.h"

#include "cvite/orc_loader.h"

#if CVITE_COMPILE_DATABASE_ENABLED
#include <clang-c/CXCompilationDatabase.h>
#endif

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <unordered_set>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kAncestorLimit = 10U;
constexpr std::size_t kLinkSearchDepth = 8U;
constexpr std::size_t kCommandOutputLimit = 1024U * 1024U;

#if CVITE_COMPILE_DATABASE_ENABLED
class CXStringOwner final {
public:
    explicit CXStringOwner(CXString value) : value_(value) {}
    CXStringOwner(const CXStringOwner &) = delete;
    CXStringOwner &operator=(const CXStringOwner &) = delete;
    ~CXStringOwner() { clang_disposeString(value_); }

    std::string str() const
    {
        const char *text = clang_getCString(value_);
        return text == nullptr ? std::string() : std::string(text);
    }

private:
    CXString value_;
};
#endif

enum class NativeInputKind {
    DynamicLibrary,
    StaticArchive,
    ObjectFile,
};

struct FileSignature final {
    std::uint64_t device = 0U;
    std::uint64_t inode = 0U;
    std::uint64_t size = 0U;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::uint64_t content_hash = 0U;
    bool is_directory = false;
    bool valid = false;
};

struct NativeInput final {
    NativeInputKind kind = NativeInputKind::DynamicLibrary;
    fs::path path;
    FileSignature signature;
};

struct TrackedPath final {
    fs::path path;
    FileSignature signature;
};

struct DiscoveredLinkCommand final {
    std::string backend;
    std::string target_name;
    fs::path working_directory;
    fs::path display_file;
    std::vector<std::string> tokens;
    std::vector<fs::path> metadata_files;
    std::size_t score = 0U;
};

struct LinkPlan final {
    std::string target_triple;
    fs::path compilation_database;
    std::string backend;
    std::string target_name;
    fs::path build_directory;
    fs::path link_command;
    std::vector<std::string> link_tokens;
    std::vector<NativeInput> inputs;
    std::vector<TrackedPath> tracked_paths;
    std::uint64_t fingerprint = 0U;
};

LinkPlan active_plan;
bool active_plan_initialized = false;

std::string trim(std::string value)
{
    const auto first = std::find_if_not(
        value.begin(), value.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        });
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), [](unsigned char character) {
            return std::isspace(character) != 0;
        }).base();
    if (first >= last) {
        return std::string();
    }
    return std::string(first, last);
}

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
        value.substr(0U, prefix.size()) == prefix;
}

bool endsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

fs::path canonicalIfPossible(const fs::path &path)
{
    std::error_code error;
    fs::path result = fs::weakly_canonical(path, error);
    if (!error) {
        return result;
    }
    result = fs::absolute(path, error);
    return error ? path.lexically_normal() : result.lexically_normal();
}


fs::path absoluteNormalized(const fs::path &path)
{
    std::error_code error;
    const fs::path result = fs::absolute(path, error);
    return error ? path.lexically_normal() : result.lexically_normal();
}

bool isRegularFile(const fs::path &path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

std::optional<std::string> readText(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream output;
    output << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return output.str();
}


std::uint64_t hashFileContents(const fs::path &path)
{
    constexpr std::uint64_t offset = UINT64_C(1469598103934665603);
    constexpr std::uint64_t prime = UINT64_C(1099511628211);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return 0U;
    }

    std::uint64_t value = offset;
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            value ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            value *= prime;
        }
    }
    return input.eof() ? value : 0U;
}

FileSignature fileSignature(const fs::path &path, bool hash_contents)
{
    FileSignature signature;
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        return signature;
    }

    signature.device = static_cast<std::uint64_t>(status.st_dev);
    signature.inode = static_cast<std::uint64_t>(status.st_ino);
    signature.size = static_cast<std::uint64_t>(status.st_size);
    signature.modified_seconds =
        static_cast<std::int64_t>(status.st_mtim.tv_sec);
    signature.modified_nanoseconds =
        static_cast<std::int64_t>(status.st_mtim.tv_nsec);
    signature.is_directory = S_ISDIR(status.st_mode);
    signature.valid = S_ISREG(status.st_mode) || signature.is_directory;
    if (signature.valid && hash_contents && !signature.is_directory) {
        signature.content_hash = hashFileContents(path);
    }
    return signature;
}

bool sameCheapSignature(
    const FileSignature &left,
    const FileSignature &right)
{
    return left.valid == right.valid && left.device == right.device &&
        left.inode == right.inode && left.size == right.size &&
        left.modified_seconds == right.modified_seconds &&
        left.modified_nanoseconds == right.modified_nanoseconds &&
        left.is_directory == right.is_directory;
}

void addTrackedPath(
    std::vector<TrackedPath> &paths,
    fs::path path,
    bool hash_contents)
{
    path = canonicalIfPossible(path);
    const auto found = std::find_if(
        paths.begin(), paths.end(), [&](const TrackedPath &tracked) {
            return tracked.path == path;
        });
    if (found != paths.end()) {
        return;
    }

    const FileSignature signature = fileSignature(path, hash_contents);
    if (signature.valid) {
        paths.push_back(TrackedPath{std::move(path), signature});
    }
}

bool trackedPathsChanged(const LinkPlan &plan)
{
    for (const TrackedPath &tracked : plan.tracked_paths) {
        if (!sameCheapSignature(
                tracked.signature, fileSignature(tracked.path, false))) {
            return true;
        }
    }
    return false;
}

std::optional<fs::path> findCompilationDatabase(const fs::path &source)
{
    fs::path directory = canonicalIfPossible(source).parent_path();
    for (std::size_t level = 0U;
         level < kAncestorLimit && !directory.empty();
         ++level) {
        const std::array<fs::path, 5U> candidates = {
            directory / "compile_commands.json",
            directory / "build" / "compile_commands.json",
            directory / "build-debug" / "compile_commands.json",
            directory / "cmake-build-debug" / "compile_commands.json",
            directory / "cmake-build-release" / "compile_commands.json",
        };
        for (const fs::path &candidate : candidates) {
            if (isRegularFile(candidate)) {
                return absoluteNormalized(candidate);
            }
        }

        std::error_code iteration_error;
        for (fs::directory_iterator iterator(
                 directory,
                 fs::directory_options::skip_permission_denied,
                 iteration_error),
             end;
             !iteration_error && iterator != end;
             iterator.increment(iteration_error)) {
            if (!iterator->is_directory(iteration_error)) {
                continue;
            }
            const std::string name = iterator->path().filename().string();
            if (!startsWith(name, "build") && !startsWith(name, "cmake-build")) {
                continue;
            }
            const fs::path candidate =
                iterator->path() / "compile_commands.json";
            if (isRegularFile(candidate)) {
                return absoluteNormalized(candidate);
            }
        }

        const fs::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return std::nullopt;
}

struct CompileDatabaseData final {
    std::vector<std::vector<std::string>> commands;
    std::vector<fs::path> sources;
    std::vector<fs::path> working_directories;
};

CompileDatabaseData readCompilationDatabase(
    const fs::path &database_path,
    const fs::path &primary_source)
{
#if CVITE_COMPILE_DATABASE_ENABLED

    CompileDatabaseData result;
    CXCompilationDatabase_Error database_error =
        CXCompilationDatabase_CanNotLoadDatabase;
    CXCompilationDatabase database = clang_CompilationDatabase_fromDirectory(
        database_path.parent_path().c_str(), &database_error);
    if (database == nullptr ||
        database_error != CXCompilationDatabase_NoError) {
        if (database != nullptr) {
            clang_CompilationDatabase_dispose(database);
        }
        return result;
    }

    CXCompileCommands commands =
        clang_CompilationDatabase_getAllCompileCommands(database);
    const unsigned command_count = clang_CompileCommands_getSize(commands);
    result.commands.reserve(static_cast<std::size_t>(command_count));
    result.sources.reserve(static_cast<std::size_t>(command_count));
    result.working_directories.reserve(
        static_cast<std::size_t>(command_count));

    for (unsigned command_index = 0U;
         command_index < command_count;
         ++command_index) {
        CXCompileCommand command =
            clang_CompileCommands_getCommand(commands, command_index);
        const std::string source_text =
            CXStringOwner(clang_CompileCommand_getFilename(command)).str();
        fs::path source_path = source_text.empty()
            ? primary_source
            : canonicalIfPossible(source_text);
        result.sources.push_back(std::move(source_path));
        const std::string directory_text =
            CXStringOwner(clang_CompileCommand_getDirectory(command)).str();
        result.working_directories.push_back(directory_text.empty()
                ? database_path.parent_path()
                : canonicalIfPossible(directory_text));

        const unsigned argument_count =
            clang_CompileCommand_getNumArgs(command);
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argument_count));
        for (unsigned argument_index = 0U;
             argument_index < argument_count;
             ++argument_index) {
            arguments.push_back(
                CXStringOwner(clang_CompileCommand_getArg(
                    command, argument_index)).str());
        }
        result.commands.push_back(std::move(arguments));
    }

    clang_CompileCommands_dispose(commands);
    clang_CompilationDatabase_dispose(database);
    return result;
#else
    (void)database_path;
    CompileDatabaseData result;
    result.sources.push_back(primary_source);
    result.working_directories.push_back(primary_source.parent_path());
    return result;
#endif
}

std::vector<std::string> tokenizeCommand(std::string_view command)
{
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool escaped = false;

    for (char character : command) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            } else {
                current.push_back(character);
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }

    if (escaped || quote != '\0') {
        return {};
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::size_t scoreLinkCommand(
    std::string_view content,
    const std::vector<fs::path> &sources)
{
    std::size_t score = 0U;
    for (const fs::path &source : sources) {
        const std::string filename = source.filename().string();
        const std::string object_name = filename + ".o";
        if (!filename.empty() && content.find(filename) != std::string_view::npos) {
            score += 1U;
        }
        if (!object_name.empty() &&
            content.find(object_name) != std::string_view::npos) {
            score += 4U;
        }
    }
    return score;
}

bool skipSearchDirectory(const fs::path &path)
{
    const std::string name = path.filename().string();
    return name == ".git" || name == ".cvite" || name == "vendor" ||
        name == "third_party" || name == "node_modules";
}

std::optional<fs::path> findCMakeLinkCommand(
    const fs::path &source,
    const std::optional<fs::path> &database,
    const std::vector<fs::path> &sources)
{
    std::vector<fs::path> roots;
    roots.push_back(canonicalIfPossible(source).parent_path());
    if (database.has_value()) {
        roots.push_back(database->parent_path());
        roots.push_back(database->parent_path().parent_path());
    }

    std::set<std::string> visited;
    std::optional<fs::path> best;
    std::size_t best_score = 0U;
    bool ambiguous = false;

    for (const fs::path &root_value : roots) {
        const fs::path root = canonicalIfPossible(root_value);
        if (!visited.insert(root.string()).second || !fs::exists(root)) {
            continue;
        }

        std::error_code error;
        fs::recursive_directory_iterator iterator(
            root,
            fs::directory_options::skip_permission_denied,
            error);
        const fs::recursive_directory_iterator end;
        while (!error && iterator != end) {
            const fs::path path = iterator->path();
            if (iterator.depth() > static_cast<int>(kLinkSearchDepth) ||
                (iterator->is_directory(error) && skipSearchDirectory(path))) {
                iterator.disable_recursion_pending();
            }

            if (iterator->is_regular_file(error) &&
                path.filename() == "link.txt" &&
                path.string().find("CMakeFiles") != std::string::npos) {
                const std::optional<std::string> content = readText(path);
                if (content.has_value()) {
                    const std::size_t score = scoreLinkCommand(*content, sources);
                    if (!best.has_value() || score > best_score) {
                        best = canonicalIfPossible(path);
                        best_score = score;
                        ambiguous = false;
                    } else if (score == best_score &&
                               canonicalIfPossible(path) != *best) {
                        ambiguous = true;
                    }
                }
            }
            iterator.increment(error);
        }
    }

    if (ambiguous && best_score == 0U) {
        std::fprintf(
            stderr,
            "[cvite] multiple CMake link commands were found; "
            "automatic native link selection is ambiguous\n");
        return std::nullopt;
    }
    return best;
}

fs::path linkWorkingDirectory(const fs::path &link_command)
{
    fs::path current = link_command.parent_path();
    while (!current.empty()) {
        if (current.filename() == "CMakeFiles") {
            return current.parent_path();
        }
        const fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return link_command.parent_path();
}

std::optional<std::string> runAndCapture(
    const std::vector<std::string> &arguments)
{
    if (arguments.empty()) {
        return std::nullopt;
    }

    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0) {
        return std::nullopt;
    }
    (void)fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return std::nullopt;
    }
    (void)posix_spawn_file_actions_adddup2(
        &actions, descriptors[1], STDOUT_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    (void)posix_spawn_file_actions_addclose(&actions, descriptors[1]);

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (const std::string &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    pid_t process = static_cast<pid_t>(0);
    const int spawn_status = posix_spawnp(
        &process,
        argv[0],
        &actions,
        nullptr,
        argv.data(),
        environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (spawn_status != 0) {
        close(descriptors[0]);
        return std::nullopt;
    }

    std::string output;
    std::array<char, 4096U> buffer{};
    while (output.size() < kCommandOutputLimit) {
        const ssize_t count = read(
            descriptors[0], buffer.data(), buffer.size());
        if (count > 0) {
            output.append(
                buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(descriptors[0]);

    int wait_status = 0;
    while (waitpid(process, &wait_status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        return std::nullopt;
    }
    return trim(std::move(output));
}

bool isLibraryToken(std::string_view token);

void appendUniquePath(std::vector<fs::path> &paths, fs::path path)
{
    path = canonicalIfPossible(path);
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(std::move(path));
    }
}

std::vector<fs::path> candidateBuildRoots(
    const fs::path &source,
    const std::optional<fs::path> &database)
{
    std::vector<fs::path> roots;
    const fs::path source_directory = canonicalIfPossible(source).parent_path();
    if (database.has_value()) {
        appendUniquePath(roots, database->parent_path());
    }
    appendUniquePath(roots, source_directory);
    appendUniquePath(roots, source_directory / "build");
    appendUniquePath(roots, source_directory / "build-debug");
    appendUniquePath(roots, source_directory / "cmake-build-debug");
    appendUniquePath(roots, source_directory / "cmake-build-release");

    std::error_code error;
    for (fs::directory_iterator iterator(
             source_directory,
             fs::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_directory(error)) {
            continue;
        }
        const std::string name = iterator->path().filename().string();
        if (startsWith(name, "build") || startsWith(name, "cmake-build")) {
            appendUniquePath(roots, iterator->path());
        }
    }
    return roots;
}

std::optional<llvm::json::Value> parseJsonFile(const fs::path &path)
{
    const std::optional<std::string> text = readText(path);
    if (!text.has_value()) {
        return std::nullopt;
    }
    llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(*text);
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        return std::nullopt;
    }
    return std::move(*parsed);
}

std::optional<fs::path> newestFileApiIndex(const fs::path &reply_directory)
{
    std::error_code error;
    std::optional<fs::path> best;
    fs::file_time_type best_time{};
    for (fs::directory_iterator iterator(
             reply_directory,
             fs::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) {
            continue;
        }
        const std::string name = iterator->path().filename().string();
        if (!startsWith(name, "index-") || !endsWith(name, ".json")) {
            continue;
        }
        const fs::file_time_type time = iterator->last_write_time(error);
        if (error) {
            break;
        }
        if (!best.has_value() || time > best_time) {
            best = canonicalIfPossible(iterator->path());
            best_time = time;
        }
    }
    return best;
}

std::optional<fs::path> codemodelFileFromIndex(
    const fs::path &index_path)
{
    std::optional<llvm::json::Value> value = parseJsonFile(index_path);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const llvm::json::Object *root = value->getAsObject();
    if (root == nullptr) {
        return std::nullopt;
    }
    const llvm::json::Array *objects = root->getArray("objects");
    if (objects == nullptr) {
        return std::nullopt;
    }
    for (const llvm::json::Value &entry_value : *objects) {
        const llvm::json::Object *entry = entry_value.getAsObject();
        if (entry == nullptr) {
            continue;
        }
        const std::optional<llvm::StringRef> kind = entry->getString("kind");
        const llvm::json::Object *version = entry->getObject("version");
        const std::optional<std::int64_t> major = version == nullptr
            ? std::nullopt
            : version->getInteger("major");
        const std::optional<llvm::StringRef> json_file =
            entry->getString("jsonFile");
        if (kind.has_value() && *kind == "codemodel" && major.has_value() &&
            *major == 2 && json_file.has_value()) {
            return canonicalIfPossible(
                index_path.parent_path() / json_file->str());
        }
    }
    return std::nullopt;
}

std::size_t scoreTargetSources(
    const llvm::json::Object &target,
    const fs::path &source_root,
    const std::vector<fs::path> &sources)
{
    const llvm::json::Array *target_sources = target.getArray("sources");
    if (target_sources == nullptr) {
        return 0U;
    }

    std::size_t score = 0U;
    for (const llvm::json::Value &source_value : *target_sources) {
        const llvm::json::Object *source_object = source_value.getAsObject();
        if (source_object == nullptr) {
            continue;
        }
        const std::optional<llvm::StringRef> path_text =
            source_object->getString("path");
        if (!path_text.has_value()) {
            continue;
        }
        fs::path target_path(path_text->str());
        if (target_path.is_relative()) {
            target_path = source_root / target_path;
        }
        target_path = canonicalIfPossible(target_path);
        for (const fs::path &source : sources) {
            const fs::path canonical_source = canonicalIfPossible(source);
            if (target_path == canonical_source) {
                score += 1000U;
            } else if (target_path.filename() == canonical_source.filename()) {
                score += 10U;
            }
        }
    }
    return score;
}

void appendCommandFragment(
    std::vector<std::string> &tokens,
    llvm::StringRef fragment)
{
    const std::string text = trim(fragment.str());
    if (text.empty()) {
        return;
    }
    std::vector<std::string> fragment_tokens = tokenizeCommand(text);
    if (fragment_tokens.empty()) {
        tokens.push_back(text);
        return;
    }
    tokens.insert(
        tokens.end(),
        std::make_move_iterator(fragment_tokens.begin()),
        std::make_move_iterator(fragment_tokens.end()));
}

std::optional<DiscoveredLinkCommand> discoverCMakeFileApi(
    const std::vector<fs::path> &build_roots,
    const std::vector<fs::path> &sources)
{
    std::optional<DiscoveredLinkCommand> best;
    bool ambiguous = false;

    for (const fs::path &build_root_value : build_roots) {
        const fs::path build_root = canonicalIfPossible(build_root_value);
        const fs::path reply_directory =
            build_root / ".cmake" / "api" / "v1" / "reply";
        const std::optional<fs::path> index =
            newestFileApiIndex(reply_directory);
        if (!index.has_value()) {
            continue;
        }
        const std::optional<fs::path> codemodel =
            codemodelFileFromIndex(*index);
        if (!codemodel.has_value()) {
            continue;
        }
        std::optional<llvm::json::Value> codemodel_value =
            parseJsonFile(*codemodel);
        if (!codemodel_value.has_value()) {
            continue;
        }
        const llvm::json::Object *codemodel_object =
            codemodel_value->getAsObject();
        if (codemodel_object == nullptr) {
            continue;
        }
        fs::path source_root = sources.empty()
            ? build_root
            : sources.front().parent_path();
        fs::path model_build_root = build_root;
        if (const llvm::json::Object *paths =
                codemodel_object->getObject("paths")) {
            if (const std::optional<llvm::StringRef> source_path =
                    paths->getString("source")) {
                source_root = canonicalIfPossible(source_path->str());
            }
            if (const std::optional<llvm::StringRef> build_path =
                    paths->getString("build")) {
                model_build_root = canonicalIfPossible(build_path->str());
            }
        }

        const llvm::json::Array *configurations =
            codemodel_object->getArray("configurations");
        if (configurations == nullptr) {
            continue;
        }
        for (const llvm::json::Value &configuration_value : *configurations) {
            const llvm::json::Object *configuration =
                configuration_value.getAsObject();
            if (configuration == nullptr) {
                continue;
            }
            const llvm::json::Array *targets =
                configuration->getArray("targets");
            if (targets == nullptr) {
                continue;
            }
            for (const llvm::json::Value &target_reference_value : *targets) {
                const llvm::json::Object *target_reference =
                    target_reference_value.getAsObject();
                if (target_reference == nullptr) {
                    continue;
                }
                const std::optional<llvm::StringRef> json_file =
                    target_reference->getString("jsonFile");
                if (!json_file.has_value()) {
                    continue;
                }
                const fs::path target_file = canonicalIfPossible(
                    codemodel->parent_path() / json_file->str());
                std::optional<llvm::json::Value> target_value =
                    parseJsonFile(target_file);
                if (!target_value.has_value()) {
                    continue;
                }
                const llvm::json::Object *target = target_value->getAsObject();
                if (target == nullptr) {
                    continue;
                }
                const std::optional<llvm::StringRef> target_type =
                    target->getString("type");
                const llvm::json::Object *link = target->getObject("link");
                if (!target_type.has_value() || !(*target_type == "EXECUTABLE") ||
                    link == nullptr) {
                    continue;
                }
                const llvm::json::Array *fragments =
                    link->getArray("commandFragments");
                if (fragments == nullptr) {
                    continue;
                }

                DiscoveredLinkCommand candidate;
                candidate.backend = "cmake-file-api";
                candidate.working_directory = model_build_root;
                candidate.display_file = target_file;
                candidate.metadata_files = {
                    reply_directory, *index, *codemodel, target_file};
                if (const std::optional<llvm::StringRef> name =
                        target->getString("name")) {
                    candidate.target_name = name->str();
                }
                candidate.score = scoreTargetSources(
                    *target, source_root, sources);
                for (const llvm::json::Value &fragment_value : *fragments) {
                    const llvm::json::Object *fragment =
                        fragment_value.getAsObject();
                    if (fragment == nullptr) {
                        continue;
                    }
                    if (const std::optional<llvm::StringRef> text =
                            fragment->getString("fragment")) {
                        appendCommandFragment(candidate.tokens, *text);
                    }
                }
                if (candidate.tokens.empty()) {
                    continue;
                }

                if (!best.has_value() || candidate.score > best->score) {
                    best = std::move(candidate);
                    ambiguous = false;
                } else if (candidate.score == best->score &&
                           candidate.display_file != best->display_file) {
                    ambiguous = true;
                }
            }
        }
    }

    if (ambiguous && best.has_value() && best->score == 0U) {
        std::fprintf(
            stderr,
            "[cvite] CMake File API exposes multiple executable targets; "
            "automatic target selection is ambiguous\n");
        return std::nullopt;
    }
    return best;
}

std::optional<DiscoveredLinkCommand> discoverCMakeLinkText(
    const fs::path &source,
    const std::optional<fs::path> &database,
    const std::vector<fs::path> &sources)
{
    const std::optional<fs::path> link_command = findCMakeLinkCommand(
        source, database, sources);
    if (!link_command.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::string> content = readText(*link_command);
    if (!content.has_value()) {
        return std::nullopt;
    }

    DiscoveredLinkCommand candidate;
    candidate.backend = "cmake-link-txt";
    candidate.working_directory = linkWorkingDirectory(*link_command);
    candidate.display_file = *link_command;
    candidate.tokens = tokenizeCommand(*content);
    candidate.metadata_files.push_back(*link_command);
    candidate.score = scoreLinkCommand(*content, sources);
    return candidate.tokens.empty()
        ? std::nullopt
        : std::optional<DiscoveredLinkCommand>(std::move(candidate));
}

void collectNinjaMetadata(
    const fs::path &path,
    std::vector<fs::path> &metadata,
    std::unordered_set<std::string> &visited)
{
    const fs::path canonical = canonicalIfPossible(path);
    if (!visited.insert(canonical.string()).second || !isRegularFile(canonical)) {
        return;
    }
    metadata.push_back(canonical);
    const std::optional<std::string> content = readText(canonical);
    if (!content.has_value()) {
        return;
    }
    std::istringstream lines(*content);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(std::move(line));
        std::string included;
        if (startsWith(line, "include ")) {
            included = trim(line.substr(std::strlen("include ")));
        } else if (startsWith(line, "subninja ")) {
            included = trim(line.substr(std::strlen("subninja ")));
        }
        if (!included.empty() && included.find('$') == std::string::npos) {
            collectNinjaMetadata(
                canonical.parent_path() / included, metadata, visited);
        }
    }
}

bool isCompileCommand(const std::vector<std::string> &tokens)
{
    return std::find(tokens.begin(), tokens.end(), "-c") != tokens.end();
}

bool outputLooksLikeExecutable(const std::vector<std::string> &tokens)
{
    for (std::size_t index = 0U; index + 1U < tokens.size(); ++index) {
        if (tokens[index] != "-o") {
            continue;
        }
        const std::string output = lower(tokens[index + 1U]);
        return !endsWith(output, ".o") && !endsWith(output, ".a") &&
            output.find(".so") == std::string::npos &&
            !endsWith(output, ".dylib");
    }
    return false;
}

std::optional<DiscoveredLinkCommand> discoverNinjaCommands(
    const std::vector<fs::path> &build_roots,
    const std::vector<fs::path> &sources)
{
    const char *ninja_environment = std::getenv("CVITE_NINJA");
    const std::string ninja = ninja_environment != nullptr &&
            ninja_environment[0] != '\0'
        ? ninja_environment
        : "ninja";
    std::optional<DiscoveredLinkCommand> best;
    bool ambiguous = false;

    for (const fs::path &build_root_value : build_roots) {
        const fs::path build_root = canonicalIfPossible(build_root_value);
        const fs::path build_file = build_root / "build.ninja";
        if (!isRegularFile(build_file)) {
            continue;
        }
        const std::optional<std::string> output = runAndCapture(
            {ninja, "-C", build_root.string(), "-t", "commands"});
        if (!output.has_value()) {
            continue;
        }

        std::istringstream lines(*output);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(std::move(line));
            if (line.empty()) {
                continue;
            }
            std::vector<std::string> tokens = tokenizeCommand(line);
            if (tokens.empty() || isCompileCommand(tokens) ||
                !outputLooksLikeExecutable(tokens)) {
                continue;
            }
            std::size_t score = scoreLinkCommand(line, sources);
            for (const std::string &token : tokens) {
                if (startsWith(token, "-l") || isLibraryToken(token)) {
                    score += 2U;
                }
            }
            if (score == 0U) {
                continue;
            }

            DiscoveredLinkCommand candidate;
            candidate.backend = "ninja-commands";
            candidate.working_directory = build_root;
            candidate.display_file = build_file;
            candidate.tokens = std::move(tokens);
            candidate.score = score;
            std::unordered_set<std::string> visited;
            collectNinjaMetadata(
                build_file, candidate.metadata_files, visited);

            if (!best.has_value() || candidate.score > best->score) {
                best = std::move(candidate);
                ambiguous = false;
            } else if (candidate.score == best->score &&
                       candidate.working_directory != best->working_directory) {
                ambiguous = true;
            }
        }
    }

    if (ambiguous) {
        std::fprintf(
            stderr,
            "[cvite] multiple Ninja executable link commands matched the "
            "project; automatic selection is ambiguous\n");
        return std::nullopt;
    }
    return best;
}

std::optional<DiscoveredLinkCommand> discoverLinkCommand(
    const fs::path &source,
    const std::optional<fs::path> &database,
    const std::vector<fs::path> &sources)
{
    const std::vector<fs::path> roots = candidateBuildRoots(source, database);
    if (std::optional<DiscoveredLinkCommand> file_api =
            discoverCMakeFileApi(roots, sources)) {
        return file_api;
    }
    if (std::optional<DiscoveredLinkCommand> link_text =
            discoverCMakeLinkText(source, database, sources)) {
        return link_text;
    }
    return discoverNinjaCommands(roots, sources);
}

std::string explicitTargetFromCommands(
    const std::vector<std::vector<std::string>> &commands)
{
    std::string selected;
    for (const std::vector<std::string> &arguments : commands) {
        for (std::size_t index = 0U; index < arguments.size(); ++index) {
            const std::string &argument = arguments[index];
            std::string candidate;
            if (startsWith(argument, "--target=")) {
                candidate = argument.substr(std::strlen("--target="));
            } else if (startsWith(argument, "-target=")) {
                candidate = argument.substr(std::strlen("-target="));
            } else if (argument == "-target" && index + 1U < arguments.size()) {
                candidate = arguments[index + 1U];
                ++index;
            } else if (argument == "-m32") {
                candidate = "i386-unknown-linux-gnu";
            } else if (argument == "-m64") {
                candidate = "x86_64-unknown-linux-gnu";
            }

            if (!candidate.empty()) {
                if (!selected.empty() && selected != candidate) {
                    return "<conflicting-targets>";
                }
                selected = std::move(candidate);
            }
        }
    }
    return selected;
}

std::string targetFamily(std::string triple)
{
    triple = lower(std::move(triple));
    const std::size_t separator = triple.find('-');
    std::string architecture = separator == std::string::npos
        ? triple
        : triple.substr(0U, separator);
    if (architecture == "amd64") {
        architecture = "x86_64";
    } else if (architecture == "arm64") {
        architecture = "aarch64";
    } else if (architecture == "i486" || architecture == "i586" ||
               architecture == "i686") {
        architecture = "i386";
    }

    std::string operating_system = "unknown";
    if (triple.find("linux") != std::string::npos) {
        operating_system = "linux";
    } else if (triple.find("darwin") != std::string::npos ||
               triple.find("apple") != std::string::npos) {
        operating_system = "darwin";
    } else if (triple.find("windows") != std::string::npos ||
               triple.find("mingw") != std::string::npos ||
               triple.find("msvc") != std::string::npos) {
        operating_system = "windows";
    } else if (triple.find("freebsd") != std::string::npos) {
        operating_system = "freebsd";
    }
    return architecture + "-" + operating_system;
}

bool isLibraryToken(std::string_view token)
{
    const std::string lowered = lower(std::string(token));
    return endsWith(lowered, ".a") || endsWith(lowered, ".o") ||
        lowered.find(".so") != std::string::npos ||
        endsWith(lowered, ".dylib");
}

bool looksLikeProjectObject(const fs::path &path)
{
    const std::string text = path.string();
    return text.find("CMakeFiles") != std::string::npos &&
        endsWith(text, ".o");
}

NativeInputKind kindForPath(const fs::path &path)
{
    const std::string text = lower(path.filename().string());
    if (endsWith(text, ".a")) {
        return NativeInputKind::StaticArchive;
    }
    if (endsWith(text, ".o")) {
        return NativeInputKind::ObjectFile;
    }
    return NativeInputKind::DynamicLibrary;
}

void appendSearchDirectory(
    std::vector<fs::path> &directories,
    const fs::path &directory,
    const fs::path &working_directory)
{
    fs::path resolved = directory;
    if (resolved.is_relative()) {
        resolved = working_directory / resolved;
    }
    resolved = canonicalIfPossible(resolved);
    if (std::find(directories.begin(), directories.end(), resolved) ==
        directories.end()) {
        directories.push_back(std::move(resolved));
    }
}

void collectSearchDirectories(
    const std::vector<std::string> &tokens,
    const fs::path &working_directory,
    std::vector<fs::path> &directories)
{
    for (std::size_t index = 0U; index < tokens.size(); ++index) {
        const std::string &token = tokens[index];
        if (token == "-L" && index + 1U < tokens.size()) {
            appendSearchDirectory(
                directories, tokens[index + 1U], working_directory);
            ++index;
        } else if (startsWith(token, "-L") && token.size() > 2U) {
            appendSearchDirectory(
                directories, token.substr(2U), working_directory);
        } else if (startsWith(token, "-Wl,")) {
            std::string payload = token.substr(4U);
            std::replace(payload.begin(), payload.end(), ',', ' ');
            const std::vector<std::string> linker_parts =
                tokenizeCommand(payload);
            for (std::size_t part = 0U; part < linker_parts.size(); ++part) {
                if ((linker_parts[part] == "-rpath" ||
                     linker_parts[part] == "-rpath-link" ||
                     linker_parts[part] == "-L") &&
                    part + 1U < linker_parts.size()) {
                    appendSearchDirectory(
                        directories,
                        linker_parts[part + 1U],
                        working_directory);
                    ++part;
                } else if (startsWith(linker_parts[part], "-L") &&
                           linker_parts[part].size() > 2U) {
                    appendSearchDirectory(
                        directories,
                        linker_parts[part].substr(2U),
                        working_directory);
                }
            }
        }
    }
}

std::optional<fs::path> compilerFileName(
    const std::string &clang_path,
    const std::string &target,
    const std::string &name)
{
    std::vector<std::string> arguments;
    arguments.push_back(clang_path);
    if (!target.empty()) {
        arguments.push_back("--target=" + target);
    }
    arguments.push_back("-print-file-name=" + name);
    const std::optional<std::string> output = runAndCapture(arguments);
    if (!output.has_value() || output->empty() || *output == name) {
        return std::nullopt;
    }
    const fs::path candidate = absoluteNormalized(*output);
    return isRegularFile(candidate)
        ? std::optional<fs::path>(candidate)
        : std::nullopt;
}

std::optional<fs::path> findVersionedSharedLibrary(
    const fs::path &directory,
    const std::string &base_name)
{
    std::error_code error;
    std::optional<fs::path> best;
    for (fs::directory_iterator iterator(
             directory,
             fs::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error) && !iterator->is_symlink(error)) {
            continue;
        }
        const std::string filename = iterator->path().filename().string();
        if (startsWith(filename, base_name + ".so.")) {
            const fs::path candidate = absoluteNormalized(iterator->path());
            if (!best.has_value() || candidate.string() < best->string()) {
                best = candidate;
            }
        }
    }
    return best;
}

std::optional<fs::path> resolveLibrary(
    const std::string &name,
    bool exact_filename,
    const std::vector<fs::path> &search_directories,
    const std::string &clang_path,
    const std::string &target)
{
    std::vector<std::string> filenames;
    if (exact_filename) {
        filenames.push_back(name);
    } else {
        filenames.push_back("lib" + name + ".so");
        filenames.push_back("lib" + name + ".dylib");
        filenames.push_back("lib" + name + ".a");
    }

    std::vector<fs::path> directories = search_directories;
    const std::array<const char *, 8U> defaults = {
        "/lib/x86_64-linux-gnu",
        "/usr/lib/x86_64-linux-gnu",
        "/lib/aarch64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu",
        "/lib64",
        "/usr/lib64",
        "/lib",
        "/usr/lib",
    };
    for (const char *directory : defaults) {
        appendSearchDirectory(directories, directory, fs::current_path());
    }

    for (const fs::path &directory : directories) {
        for (const std::string &filename : filenames) {
            const fs::path candidate = directory / filename;
            if (isRegularFile(candidate)) {
                return absoluteNormalized(candidate);
            }
        }
        if (!exact_filename) {
            const std::optional<fs::path> versioned =
                findVersionedSharedLibrary(directory, "lib" + name);
            if (versioned.has_value()) {
                return versioned;
            }
        }
    }

    for (const std::string &filename : filenames) {
        const std::optional<fs::path> candidate = compilerFileName(
            clang_path, target, filename);
        if (candidate.has_value()) {
            return candidate;
        }
    }
    return std::nullopt;
}

void addInput(
    std::vector<NativeInput> &inputs,
    NativeInputKind kind,
    fs::path path)
{
    path = absoluteNormalized(path);
    if (!isRegularFile(path)) {
        return;
    }
    const auto duplicate = std::find_if(
        inputs.begin(), inputs.end(), [&](const NativeInput &input) {
            return input.path == path;
        });
    if (duplicate == inputs.end()) {
        inputs.push_back(NativeInput{kind, std::move(path), FileSignature{}});
    }
}

void collectNativeInputs(
    const std::vector<std::string> &tokens,
    const fs::path &working_directory,
    const std::vector<fs::path> &search_directories,
    const std::string &clang_path,
    const std::string &target,
    std::vector<NativeInput> &inputs)
{
    bool skip_next_output = false;
    for (std::size_t index = 0U; index < tokens.size(); ++index) {
        const std::string &token = tokens[index];
        if (skip_next_output) {
            skip_next_output = false;
            continue;
        }
        if (token == "-o") {
            skip_next_output = true;
            continue;
        }
        if (token == "-L") {
            ++index;
            continue;
        }
        if (startsWith(token, "-L") || startsWith(token, "-Wl,")) {
            continue;
        }

        std::string library_name;
        bool exact_filename = false;
        if (token == "-l" && index + 1U < tokens.size()) {
            library_name = tokens[++index];
        } else if (startsWith(token, "-l:") && token.size() > 3U) {
            library_name = token.substr(3U);
            exact_filename = true;
        } else if (startsWith(token, "-l") && token.size() > 2U) {
            library_name = token.substr(2U);
        } else if (token == "-pthread") {
            library_name = "pthread";
        }

        if (!library_name.empty()) {
            const std::optional<fs::path> resolved = resolveLibrary(
                library_name,
                exact_filename,
                search_directories,
                clang_path,
                target);
            if (resolved.has_value()) {
                addInput(inputs, kindForPath(*resolved), *resolved);
            } else {
                std::fprintf(
                    stderr,
                    "[cvite] warning: could not resolve native library -l%s\n",
                    library_name.c_str());
            }
            continue;
        }

        if (!isLibraryToken(token)) {
            continue;
        }
        fs::path path(token);
        if (path.is_relative()) {
            path = working_directory / path;
        }
        path = absoluteNormalized(path);
        if (looksLikeProjectObject(path)) {
            continue;
        }
        addInput(inputs, kindForPath(path), std::move(path));
    }
}

std::uint64_t fnvAppend(std::uint64_t value, std::string_view text)
{
    constexpr std::uint64_t prime = UINT64_C(1099511628211);
    for (char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        value ^= static_cast<std::uint64_t>(byte);
        value *= prime;
    }
    value ^= UINT64_C(255);
    value *= prime;
    return value;
}

std::uint64_t planFingerprint(const LinkPlan &plan)
{
    std::uint64_t value = UINT64_C(1469598103934665603);
    value = fnvAppend(value, plan.target_triple);
    value = fnvAppend(value, plan.backend);
    value = fnvAppend(value, plan.target_name);
    for (const std::string &token : plan.link_tokens) {
        value = fnvAppend(value, token);
    }
    std::vector<std::string> records;
    records.reserve(plan.inputs.size());
    for (const NativeInput &input : plan.inputs) {
        records.push_back(
            std::to_string(static_cast<unsigned>(input.kind)) + ":" +
            input.path.string() + ":" +
            std::to_string(input.signature.content_hash));
    }
    std::sort(records.begin(), records.end());
    for (const std::string &record : records) {
        value = fnvAppend(value, record);
    }
    return value;
}

void collectEnvironmentPreloads(std::vector<NativeInput> &inputs)
{
    const char *environment = std::getenv("CVITE_PRELOAD");
    if (environment == nullptr || environment[0] == '\0') {
        return;
    }

    std::string value(environment);
    std::size_t start = 0U;
    while (start <= value.size()) {
        const std::size_t separator = value.find(':', start);
        const std::size_t length = separator == std::string::npos
            ? value.size() - start
            : separator - start;
        const std::string item = value.substr(start, length);
        if (!item.empty()) {
            addInput(
                inputs,
                NativeInputKind::DynamicLibrary,
                absoluteNormalized(item));
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1U;
    }
}

void finalizePlan(LinkPlan &plan)
{
    std::sort(
        plan.inputs.begin(),
        plan.inputs.end(),
        [](const NativeInput &left, const NativeInput &right) {
            if (left.path != right.path) {
                return left.path.string() < right.path.string();
            }
            return static_cast<unsigned>(left.kind) <
                static_cast<unsigned>(right.kind);
        });

    for (NativeInput &input : plan.inputs) {
        input.signature = fileSignature(input.path, true);
        if (input.signature.valid) {
            plan.tracked_paths.push_back(
                TrackedPath{input.path, input.signature});
        }
    }
    if (!plan.compilation_database.empty()) {
        addTrackedPath(plan.tracked_paths, plan.compilation_database, true);
    }
    plan.fingerprint = planFingerprint(plan);
}

LinkPlan discoverPlan(
    const fs::path &source,
    const std::string &clang_path)
{
    LinkPlan plan;
    const fs::path canonical_source = canonicalIfPossible(source);
    const std::optional<fs::path> database =
        findCompilationDatabase(canonical_source);
    CompileDatabaseData database_data;
    if (database.has_value()) {
        plan.compilation_database = *database;
        database_data = readCompilationDatabase(*database, canonical_source);
    }
    if (database_data.sources.empty()) {
        database_data.sources.push_back(canonical_source);
        database_data.working_directories.push_back(
            canonical_source.parent_path());
    }

    std::string explicit_target =
        explicitTargetFromCommands(database_data.commands);
    if (explicit_target == "<conflicting-targets>") {
        plan.target_triple = explicit_target;
    } else if (!explicit_target.empty()) {
        const std::optional<std::string> canonical_target = runAndCapture(
            {clang_path, "--target=" + explicit_target, "-print-target-triple"});
        plan.target_triple = canonical_target.has_value()
            ? *canonical_target
            : explicit_target;
    } else if (const std::optional<std::string> default_target =
                   runAndCapture({clang_path, "-print-target-triple"})) {
        plan.target_triple = *default_target;
    }

    std::vector<fs::path> search_directories;
    for (std::size_t index = 0U;
         index < database_data.commands.size();
         ++index) {
        const fs::path working_directory =
            index < database_data.working_directories.size()
            ? database_data.working_directories[index]
            : canonical_source.parent_path();
        collectSearchDirectories(
            database_data.commands[index],
            working_directory,
            search_directories);
        collectNativeInputs(
            database_data.commands[index],
            working_directory,
            search_directories,
            clang_path,
            plan.target_triple,
            plan.inputs);
    }

    if (std::optional<DiscoveredLinkCommand> link = discoverLinkCommand(
            canonical_source, database, database_data.sources)) {
        plan.backend = link->backend;
        plan.target_name = link->target_name;
        plan.build_directory = link->working_directory;
        plan.link_command = link->display_file;
        plan.link_tokens = link->tokens;
        for (const fs::path &metadata : link->metadata_files) {
            addTrackedPath(plan.tracked_paths, metadata, true);
        }
        collectSearchDirectories(
            link->tokens, link->working_directory, search_directories);
        collectNativeInputs(
            link->tokens,
            link->working_directory,
            search_directories,
            clang_path,
            plan.target_triple,
            plan.inputs);
    }

    collectEnvironmentPreloads(plan.inputs);
    finalizePlan(plan);
    return plan;
}

int reportLoaderError(const char *operation, const cvite_error &error)
{
    std::fprintf(
        stderr,
        "[cvite] %s failed (%s): %s\n",
        operation,
        cvite_status_string(error.status),
        error.message[0] == '\0' ? "no diagnostic was provided" : error.message);
    return -1;
}

int validateTarget(
    cvite_orc_loader *loader,
    const LinkPlan &plan)
{
    const char *host_value = cvite_orc_loader_target_triple(loader);
    const std::string host = host_value == nullptr
        ? std::string()
        : std::string(host_value);
    if (plan.target_triple == "<conflicting-targets>") {
        std::fprintf(
            stderr,
            "[cvite] compile_commands.json contains conflicting native targets; "
            "one in-process JIT cannot execute all of them\n");
        return -1;
    }
    if (!plan.target_triple.empty() && !host.empty() &&
        targetFamily(plan.target_triple) != targetFamily(host)) {
        std::fprintf(
            stderr,
            "[cvite] project target %s is incompatible with the in-process "
            "JIT target %s\n",
            plan.target_triple.c_str(),
            host.c_str());
        return -1;
    }
    return 0;
}

int loadInputs(cvite_orc_loader *loader, const LinkPlan &plan)
{
    cvite_error error{};
    for (const NativeInput &input : plan.inputs) {
        cvite_status status = CVITE_STATUS_INVALID_ARGUMENT;
        const char *operation = "native link input";
        switch (input.kind) {
        case NativeInputKind::DynamicLibrary:
            operation = "dynamic-library loading";
            status = cvite_orc_loader_load_dynamic_library(
                loader, input.path.c_str(), &error);
            break;
        case NativeInputKind::StaticArchive:
            operation = "static-archive linking";
            status = cvite_orc_loader_link_static_archive(
                loader, input.path.c_str(), &error);
            break;
        case NativeInputKind::ObjectFile:
            operation = "support-object linking";
            status = cvite_orc_loader_add_support_object(
                loader, input.path.c_str(), &error);
            break;
        }
        if (status != CVITE_STATUS_OK) {
            return reportLoaderError(operation, error);
        }
        std::fprintf(
            stderr,
            "[cvite] native link input: %s\n",
            input.path.c_str());
    }
    return 0;
}

bool automaticRestartDisabled()
{
    const char *value = std::getenv("CVITE_DISABLE_AUTO_RESTART");
    return value != nullptr && value[0] != '\0' &&
        std::strcmp(value, "0") != 0;
}

int restartCurrentProcess(const char *reason)
{
    if (automaticRestartDisabled()) {
        std::fprintf(
            stderr,
            "[cvite] %s; automatic restart is disabled\n",
            reason);
        return -1;
    }

    const std::optional<std::string> command_line =
        readText("/proc/self/cmdline");
    if (!command_line.has_value() || command_line->empty()) {
        std::fprintf(
            stderr,
            "[cvite] %s; could not reconstruct /proc/self/cmdline\n",
            reason);
        return -1;
    }

    std::vector<char> storage(command_line->begin(), command_line->end());
    if (storage.empty() || storage.back() != '\0') {
        storage.push_back('\0');
    }
    std::vector<char *> arguments;
    for (std::size_t offset = 0U; offset < storage.size();) {
        char *argument = storage.data() + offset;
        const std::size_t length = std::strlen(argument);
        if (length == 0U) {
            break;
        }
        arguments.push_back(argument);
        offset += length + 1U;
    }
    arguments.push_back(nullptr);

    std::array<char, 4096U> executable{};
    const ssize_t length = readlink(
        "/proc/self/exe", executable.data(), executable.size() - 1U);
    if (length <= 0 ||
        static_cast<std::size_t>(length) >= executable.size()) {
        std::fprintf(
            stderr,
            "[cvite] %s; could not resolve /proc/self/exe\n",
            reason);
        return -1;
    }
    executable[static_cast<std::size_t>(length)] = '\0';

    std::fprintf(stderr, "[cvite] %s; restarting the process\n", reason);
    std::fflush(nullptr);
    execv(executable.data(), arguments.data());
    std::fprintf(
        stderr,
        "[cvite] restart failed: %s\n",
        std::strerror(errno));
    return -1;
}

bool sameInputIdentity(const NativeInput &left, const NativeInput &right)
{
    return left.kind == right.kind && left.path == right.path;
}

bool nativeInputContentsChanged(
    const LinkPlan &previous,
    const LinkPlan &candidate)
{
    if (previous.inputs.size() != candidate.inputs.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < previous.inputs.size(); ++index) {
        if (!sameInputIdentity(previous.inputs[index], candidate.inputs[index])) {
            return false;
        }
        if (previous.inputs[index].signature.content_hash !=
            candidate.inputs[index].signature.content_hash) {
            return true;
        }
    }
    return false;
}

int checkPlan(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path)
{
    if (!active_plan_initialized || loader == nullptr ||
        source_path == nullptr || clang_path == nullptr) {
        return 0;
    }
    LinkPlan candidate = discoverPlan(source_path, clang_path);
    if (validateTarget(loader, candidate) != 0) {
        return restartCurrentProcess("the project target changed");
    }
    if (candidate.fingerprint != active_plan.fingerprint) {
        const char *reason = nativeInputContentsChanged(active_plan, candidate)
            ? "a native link input changed"
            : "the native link plan changed";
        return restartCurrentProcess(reason);
    }
    active_plan = std::move(candidate);
    return 0;
}

} // namespace

extern "C" int cvite_restart_current_process(const char *reason)
{
    const char *effective_reason =
        reason != nullptr && reason[0] != '\0'
        ? reason
        : "native state is incompatible with the candidate";
    return restartCurrentProcess(effective_reason);
}

extern "C" int cvite_native_link_prepare(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path)
{
    if (loader == nullptr || source_path == nullptr || clang_path == nullptr) {
        return -1;
    }
    LinkPlan plan = discoverPlan(source_path, clang_path);
    if (validateTarget(loader, plan) != 0 || loadInputs(loader, plan) != 0) {
        return -1;
    }
    active_plan = std::move(plan);
    active_plan_initialized = true;
    if (!active_plan.target_triple.empty()) {
        std::fprintf(
            stderr,
            "[cvite] project target: %s\n",
            active_plan.target_triple.c_str());
    }
    if (!active_plan.backend.empty()) {
        std::fprintf(
            stderr,
            "[cvite] native link backend: %s%s%s\n",
            active_plan.backend.c_str(),
            active_plan.target_name.empty() ? "" : " (target ",
            active_plan.target_name.empty()
                ? ""
                : (active_plan.target_name + ")").c_str());
    }
    if (!active_plan.link_command.empty()) {
        std::fprintf(
            stderr,
            "[cvite] native link metadata: %s\n",
            active_plan.link_command.c_str());
    }
    return 0;
}

extern "C" int cvite_native_link_check(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path)
{
    return checkPlan(loader, source_path, clang_path);
}

extern "C" int cvite_native_link_poll(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path)
{
    if (!active_plan_initialized || !trackedPathsChanged(active_plan)) {
        return 0;
    }
    return checkPlan(loader, source_path, clang_path);
}
