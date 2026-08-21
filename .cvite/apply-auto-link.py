from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    raise SystemExit(message)


def write(path: str, content: str) -> None:
    destination = ROOT / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(content, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    destination = ROOT / path
    text = destination.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        fail(f"{path}: expected one occurrence of marker, found {count}: {old!r}")
    destination.write_text(text.replace(old, new, 1), encoding="utf-8")


def insert_before_last(path: str, marker: str, insertion: str) -> None:
    destination = ROOT / path
    text = destination.read_text(encoding="utf-8")
    position = text.rfind(marker)
    if position < 0:
        fail(f"{path}: marker not found: {marker!r}")
    destination.write_text(text[:position] + insertion + text[position:], encoding="utf-8")


native_link_cpp = r'''#include "run_internal.h"

#include "cvite/orc_loader.h"

#include <clang-c/CXCompilationDatabase.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

enum class NativeInputKind {
    DynamicLibrary,
    StaticArchive,
    ObjectFile,
};

struct NativeInput final {
    NativeInputKind kind = NativeInputKind::DynamicLibrary;
    fs::path path;
};

struct LinkPlan final {
    std::string target_triple;
    fs::path compilation_database;
    fs::path link_command;
    fs::file_time_type link_command_time{};
    std::vector<NativeInput> inputs;
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
                return canonicalIfPossible(candidate);
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
                return canonicalIfPossible(candidate);
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
};

CompileDatabaseData readCompilationDatabase(
    const fs::path &database_path,
    const fs::path &primary_source)
{
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
    if (pipe2(descriptors, O_CLOEXEC) != 0) {
        return std::nullopt;
    }

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
            const std::vector<std::string> parts = tokenizeCommand(
                std::string(token.substr(4U)).replace(
                    0U, 0U, ""));
            (void)parts;
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
    const fs::path candidate = canonicalIfPossible(*output);
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
            const fs::path candidate = canonicalIfPossible(iterator->path());
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
                return canonicalIfPossible(candidate);
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
    path = canonicalIfPossible(path);
    if (!isRegularFile(path)) {
        return;
    }
    const auto duplicate = std::find_if(
        inputs.begin(), inputs.end(), [&](const NativeInput &input) {
            return input.path == path;
        });
    if (duplicate == inputs.end()) {
        inputs.push_back(NativeInput{kind, std::move(path)});
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
        path = canonicalIfPossible(path);
        if (looksLikeProjectObject(path)) {
            continue;
        }
        addInput(inputs, kindForPath(path), std::move(path));
    }
}

std::uint64_t fnvAppend(std::uint64_t value, std::string_view text)
{
    constexpr std::uint64_t prime = UINT64_C(1099511628211);
    for (unsigned char byte : text) {
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
    std::vector<std::string> records;
    records.reserve(plan.inputs.size());
    for (const NativeInput &input : plan.inputs) {
        records.push_back(
            std::to_string(static_cast<unsigned>(input.kind)) + ":" +
            input.path.string());
    }
    std::sort(records.begin(), records.end());
    for (const std::string &record : records) {
        value = fnvAppend(value, record);
    }
    return value;
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
    }

    std::vector<fs::path> search_directories;
    for (const std::vector<std::string> &command : database_data.commands) {
        collectSearchDirectories(
            command,
            canonical_source.parent_path(),
            search_directories);
        collectNativeInputs(
            command,
            canonical_source.parent_path(),
            search_directories,
            clang_path,
            explicit_target,
            plan.inputs);
    }

    const std::optional<fs::path> link_command = findCMakeLinkCommand(
        canonical_source, database, database_data.sources);
    if (link_command.has_value()) {
        plan.link_command = *link_command;
        std::error_code time_error;
        plan.link_command_time = fs::last_write_time(*link_command, time_error);
        const std::optional<std::string> content = readText(*link_command);
        if (content.has_value()) {
            const std::vector<std::string> tokens = tokenizeCommand(*content);
            const fs::path working_directory =
                linkWorkingDirectory(*link_command);
            collectSearchDirectories(
                tokens, working_directory, search_directories);
            collectNativeInputs(
                tokens,
                working_directory,
                search_directories,
                clang_path,
                explicit_target,
                plan.inputs);
        }
    }

    plan.fingerprint = planFingerprint(plan);
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
        return restartCurrentProcess("the native link plan changed");
    }
    active_plan.link_command_time = candidate.link_command_time;
    return 0;
}

} // namespace

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
    if (!active_plan.link_command.empty()) {
        std::fprintf(
            stderr,
            "[cvite] CMake link command: %s\n",
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
    if (!active_plan_initialized || active_plan.link_command.empty()) {
        return 0;
    }
    std::error_code error;
    const fs::file_time_type current = fs::last_write_time(
        active_plan.link_command, error);
    if (error || current == active_plan.link_command_time) {
        return 0;
    }
    return checkPlan(loader, source_path, clang_path);
}
'''

write("src/cli/native_link.cpp", native_link_cpp)

orc_header = ROOT / "include/cvite/orc_loader.h"
header_text = orc_header.read_text(encoding="utf-8")
if "cvite_orc_loader_load_dynamic_library" not in header_text:
    insertion = r'''
/* Native link inputs discovered by the development server. */
const char *cvite_orc_loader_target_triple(const cvite_orc_loader *loader);

cvite_status cvite_orc_loader_load_dynamic_library(
    cvite_orc_loader *loader,
    const char *path,
    cvite_error *error);

cvite_status cvite_orc_loader_link_static_archive(
    cvite_orc_loader *loader,
    const char *path,
    cvite_error *error);

cvite_status cvite_orc_loader_add_support_object(
    cvite_orc_loader *loader,
    const char *path,
    cvite_error *error);

'''
    insert_before_last(
        "include/cvite/orc_loader.h", "#ifdef __cplusplus", insertion)

orc_path = ROOT / "src/host/orc_loader.cpp"
orc_text = orc_path.read_text(encoding="utf-8")
if "automatic_link_dylibs" not in orc_text:
    replace_once(
        "src/host/orc_loader.cpp",
        "struct cvite_orc_loader {\n",
        "struct cvite_orc_loader {\n"
        "    std::vector<llvm::orc::JITDylib *> automatic_link_dylibs;\n"
        "    std::string automatic_target_triple;\n"
        "    std::uint64_t next_automatic_link_id = 1U;\n",
    )
    replace_once(
        "src/host/orc_loader.cpp",
        "    created->jit = std::move(*jit);\n",
        "    created->jit = std::move(*jit);\n"
        "    created->automatic_target_triple =\n"
        "        created->jit->getTargetTriple().str();\n",
    )
    replace_once(
        "src/host/orc_loader.cpp",
        "    llvm::orc::JITDylib &candidate_dylib = *dylib;\n",
        "    llvm::orc::JITDylib &candidate_dylib = *dylib;\n"
        "    for (llvm::orc::JITDylib *native_dylib :\n"
        "         loader->automatic_link_dylibs) {\n"
        "        candidate_dylib.addToLinkOrder(\n"
        "            *native_dylib,\n"
        "            llvm::orc::JITDylibLookupFlags::MatchAllSymbols);\n"
        "    }\n",
    )

    orc_append = r'''

extern "C" const char *cvite_orc_loader_target_triple(
    const cvite_orc_loader *loader)
{
    return loader == nullptr ? nullptr : loader->automatic_target_triple.c_str();
}

extern "C" cvite_status cvite_orc_loader_load_dynamic_library(
    cvite_orc_loader *loader,
    const char *path,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || path == nullptr || path[0] == '\0') {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid dynamic-library request");
    }

    std::lock_guard<std::mutex> guard(loader->mutex);
    if (!loader->generations.empty()) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "native libraries must be loaded before the baseline is staged");
    }

    auto dylib = loader->jit->loadPlatformDynamicLibrary(path);
    if (!dylib) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            dylib.takeError(),
            "could not load native dynamic library");
    }
    loader->automatic_link_dylibs.push_back(&*dylib);
    return CVITE_STATUS_OK;
}

extern "C" cvite_status cvite_orc_loader_link_static_archive(
    cvite_orc_loader *loader,
    const char *path,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || path == nullptr || path[0] == '\0') {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid static-archive request");
    }

    std::lock_guard<std::mutex> guard(loader->mutex);
    if (!loader->generations.empty()) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "native archives must be linked before the baseline is staged");
    }

    const std::string name = "cvite.native.archive." +
        std::to_string(loader->next_automatic_link_id++);
    auto dylib = loader->jit->createJITDylib(name);
    if (!dylib) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            dylib.takeError(),
            "could not create a native archive namespace");
    }
    if (llvm::Error link_error =
            loader->jit->linkStaticLibraryInto(*dylib, path)) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            std::move(link_error),
            "could not link native static archive");
    }
    loader->automatic_link_dylibs.push_back(&*dylib);
    return CVITE_STATUS_OK;
}

extern "C" cvite_status cvite_orc_loader_add_support_object(
    cvite_orc_loader *loader,
    const char *path,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || path == nullptr || path[0] == '\0') {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid support-object request");
    }

    std::lock_guard<std::mutex> guard(loader->mutex);
    if (!loader->generations.empty()) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "support objects must be linked before the baseline is staged");
    }

    auto object = llvm::MemoryBuffer::getFile(path, false, false);
    if (!object) {
        return fail(
            error,
            CVITE_STATUS_IO_ERROR,
            "could not read support object '" + std::string(path) +
                "': " + object.getError().message());
    }

    const std::string name = "cvite.native.object." +
        std::to_string(loader->next_automatic_link_id++);
    auto dylib = loader->jit->createJITDylib(name);
    if (!dylib) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            dylib.takeError(),
            "could not create a support-object namespace");
    }
    if (llvm::Error add_error =
            loader->jit->addObjectFile(*dylib, std::move(*object))) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            std::move(add_error),
            "could not link support object");
    }
    loader->automatic_link_dylibs.push_back(&*dylib);
    return CVITE_STATUS_OK;
}
'''
    with orc_path.open("a", encoding="utf-8") as output:
        output.write(orc_append)

run_header = ROOT / "src/cli/run_internal.h"
run_header_text = run_header.read_text(encoding="utf-8")
if "cvite_native_link_prepare" not in run_header_text:
    insertion = r'''
#ifdef __cplusplus
extern "C" {
#endif

int cvite_native_link_prepare(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path);

int cvite_native_link_check(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path);

int cvite_native_link_poll(
    cvite_orc_loader *loader,
    const char *source_path,
    const char *clang_path);

#ifdef __cplusplus
}
#endif

'''
    insert_before_last("src/cli/run_internal.h", "#endif", insertion)

run_path = ROOT / "src/cli/run.c"
run_text = run_path.read_text(encoding="utf-8")
if "cvite_native_link_prepare(" not in run_text:
    marker = "    status = cvite_orc_loader_stage_object(\n"
    position = run_text.find(marker)
    if position < 0:
        fail("src/cli/run.c: baseline stage marker not found")
    insertion = (
        "    if (cvite_native_link_prepare(\n"
        "            state->loader, state->source_path, state->clang_path) != 0) {\n"
        "        cvite_remove_if_present(object_path);\n"
        "        return -1;\n"
        "    }\n\n"
    )
    run_path.write_text(
        run_text[:position] + insertion + run_text[position:],
        encoding="utf-8",
    )

watcher_path = ROOT / "src/cli/watcher.c"
watcher_text = watcher_path.read_text(encoding="utf-8")
if "cvite_native_link_check(" not in watcher_text:
    marker = "static int publish_candidate(cvite_run_state *state)\n{\n"
    replacement = (
        marker
        + "    if (cvite_native_link_check(\n"
        + "            state->loader, state->source_path, state->clang_path) != 0) {\n"
        + "        return -1;\n"
        + "    }\n\n"
    )
    if marker not in watcher_text:
        fail("src/cli/watcher.c: publish_candidate marker not found")
    watcher_text = watcher_text.replace(marker, replacement, 1)

    timeout_marker = "        if (poll_status == 0) {\n            continue;\n        }\n"
    timeout_replacement = (
        "        if (poll_status == 0) {\n"
        "            (void)cvite_native_link_poll(\n"
        "                state->loader, state->source_path, state->clang_path);\n"
        "            continue;\n"
        "        }\n"
    )
    if timeout_marker not in watcher_text:
        fail("src/cli/watcher.c: poll timeout marker not found")
    watcher_text = watcher_text.replace(
        timeout_marker, timeout_replacement, 1)
    watcher_path.write_text(watcher_text, encoding="utf-8")

cmake_path = ROOT / "CMakeLists.txt"
cmake_text = cmake_path.read_text(encoding="utf-8")
if "src/cli/native_link.cpp" not in cmake_text:
    marker = "        src/cli/compile_database.c\n"
    if marker not in cmake_text:
        marker = "        src/cli/toolchain.c\n"
    if marker not in cmake_text:
        fail("CMakeLists.txt: runner source marker not found")
    cmake_text = cmake_text.replace(
        marker, marker + "        src/cli/native_link.cpp\n", 1)

if "run-auto-native-link" not in cmake_text:
    test_block = r'''

if(CVITE_BUILD_LLVM_PASS AND CVITE_BUILD_ORC_LOADER AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_library(cvite_test_auto_native SHARED
        tests/fixtures/auto_link_library.c
    )
    set_target_properties(cvite_test_auto_native PROPERTIES
        OUTPUT_NAME cvite_auto_native
    )

    add_test(
        NAME run-auto-native-link
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_auto_link.py
            $<TARGET_FILE:cvite>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/auto_link_app.c
            $<TARGET_FILE:cvite_test_auto_native>
    )
    set_tests_properties(run-auto-native-link PROPERTIES TIMEOUT 60)
endif()
'''
    marker = "\nif(CVITE_BUILD_EXAMPLES)\n"
    if marker not in cmake_text:
        fail("CMakeLists.txt: example section marker not found")
    cmake_text = cmake_text.replace(marker, test_block + marker, 1)

cmake_path.write_text(cmake_text, encoding="utf-8")

write(
    "tests/fixtures/auto_link_library.c",
    r'''int cvite_auto_native_bonus(void)
{
    return 3;
}
''',
)

write(
    "tests/fixtures/auto_link_app.c",
    r'''#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <time.h>

int cvite_auto_native_bonus(void);

static int total = 0;

static int step(void)
{
    total += cvite_auto_native_bonus();
    return total;
}

int main(void)
{
    const struct timespec pause = {0, 20000000L};
    for (int iteration = 0; iteration < 450; ++iteration) {
        (void)printf("value=%d\n", step());
        (void)fflush(stdout);
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}
''',
)

write(
    "tests/check_auto_link.py",
    r'''#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

VALUE = re.compile(r"^value=(\d+)$")


def fail(message: str, output: list[str]) -> None:
    joined = "".join(output[-160:])
    raise AssertionError(f"{message}\n\n--- cvite output ---\n{joined}")


def atomic_write(path: Path, content: str) -> None:
    replacement = path.with_suffix(path.suffix + ".new")
    replacement.write_text(content, encoding="utf-8")
    os.replace(replacement, path)


def library_link_name(path: Path) -> str:
    name = path.name
    if name.startswith("lib"):
        name = name[3:]
    marker = name.find(".so")
    if marker >= 0:
        name = name[:marker]
    elif name.endswith(".dylib"):
        name = name[:-6]
    return name


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: check_auto_link.py <cvite> <app-fixture> <shared-library>"
        )

    cvite = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    library = Path(sys.argv[3]).resolve()
    output: list[str] = []

    with tempfile.TemporaryDirectory(prefix="cvite-auto-link-") as directory:
        root = Path(directory)
        source = root / "main.c"
        build = root / "build"
        object_directory = build / "CMakeFiles" / "app.dir"
        object_directory.mkdir(parents=True)
        shutil.copyfile(fixture, source)

        original = source.read_text(encoding="utf-8")
        edited = original.replace(
            "total += cvite_auto_native_bonus();",
            "total += cvite_auto_native_bonus() * 10;",
            1,
        )
        if edited == original:
            fail("fixture edit marker was not found", output)

        compile_commands = [
            {
                "directory": str(build),
                "arguments": [
                    "clang-18",
                    "-std=c11",
                    "-O2",
                    "-c",
                    str(source),
                    "-o",
                    str(object_directory / "main.c.o"),
                ],
                "file": str(source),
            }
        ]
        (build / "compile_commands.json").write_text(
            json.dumps(compile_commands), encoding="utf-8"
        )

        link_name = library_link_name(library)
        link_command = (
            f"clang-18 CMakeFiles/app.dir/main.c.o "
            f"-L{library.parent} -l{link_name} "
            f"-Wl,-rpath,{library.parent} -o app\n"
        )
        (object_directory / "link.txt").write_text(
            link_command, encoding="utf-8"
        )

        environment = os.environ.copy()
        environment.pop("CVITE_PRELOAD", None)
        environment["CVITE_VERBOSE"] = "1"
        process = subprocess.Popen(
            [str(cvite), "run", str(root)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None

        values: list[int] = []
        edited_written = False
        refreshed = False
        preserved_jump = False
        automatic_input_seen = False
        deadline = time.monotonic() + 45.0

        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([process.stdout], [], [], 0.25)
                if not ready:
                    if process.poll() is not None:
                        break
                    continue
                line = process.stdout.readline()
                if line == "":
                    if process.poll() is not None:
                        break
                    continue
                output.append(line)

                if "native link input:" in line and str(library.parent) in line:
                    automatic_input_seen = True
                if "[cvite] refreshed" in line:
                    refreshed = True

                match = VALUE.match(line.rstrip("\n"))
                if match:
                    current = int(match.group(1))
                    if values and edited_written and current - values[-1] >= 30:
                        preserved_jump = True
                    values.append(current)
                    if len(values) >= 5 and not edited_written:
                        atomic_write(source, edited)
                        edited_written = True

                if (
                    automatic_input_seen
                    and refreshed
                    and preserved_jump
                    and len(values) >= 12
                ):
                    break
        finally:
            try:
                return_code = process.wait(timeout=15.0)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    return_code = process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait(timeout=3.0)

        if return_code != 0:
            fail(f"cvite exited with status {return_code}", output)
        if not automatic_input_seen:
            fail("automatic native link input was not reported", output)
        if not refreshed or not preserved_jump:
            fail("did not observe state-preserving refresh", output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
''',
)

docs_path = ROOT / "docs/dev-server.md"
docs = docs_path.read_text(encoding="utf-8")
if "## Automatic native link discovery" not in docs:
    docs += r'''

## Automatic native link discovery

When `compile_commands.json` belongs to a CMake build, CVite discovers the
matching `CMakeFiles/<target>.dir/link.txt` command and reconstructs the native
support inputs needed by the in-process program. The discovery layer recognizes
`-L`, `-l`, direct shared-library paths, static archives, support object files,
and CMake rpath search directories.

Dynamic libraries are loaded into an LLVM ORC platform JITDylib. Static archives
and standalone support objects are linked into dedicated ORC namespaces. Every
baseline and candidate generation receives those namespaces in its link order,
so external symbol addresses remain stable across refreshes.

CVite also reads explicit target options from the Clang compilation database.
A target whose architecture or operating-system family differs from the host JIT
is rejected before native code is published. One in-process CVite session cannot
execute a foreign architecture.

The native link plan is fingerprinted. If CMake rewrites `link.txt`, or project
target/library options change in `compile_commands.json`, CVite classifies that
as a red refresh and re-executes itself so the edited program starts with a fresh
baseline and a coherent native symbol graph. Set
`CVITE_DISABLE_AUTO_RESTART=1` to inspect the rejection without restarting.

`CVITE_PRELOAD` remains available as an explicit override for non-CMake builds,
plugin systems, and libraries whose ownership cannot be inferred from a build
command.
'''
    docs_path.write_text(docs, encoding="utf-8")

print("automatic native link discovery staged")
