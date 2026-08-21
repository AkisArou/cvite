#include "semantic_cache.h"

#include "semantic_index.h"

#include <clang-c/CXCompilationDatabase.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct CacheEntry final {
    std::string active_index;
    std::string staged_index;
};

std::mutex cache_mutex;
std::map<std::string, CacheEntry> cache_entries;

std::string takeString(CXString value)
{
    const char *text = clang_getCString(value);
    std::string result = text == nullptr ? std::string() : std::string(text);
    clang_disposeString(value);
    return result;
}

bool traceEnabled()
{
    const char *value = std::getenv("CVITE_TRACE_SEMANTICS");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void trace(const std::string &message)
{
    if (traceEnabled()) {
        std::fprintf(stderr, "[cvite] semantic cache: %s\n", message.c_str());
    }
}

std::string indexPath(const char *ir_path)
{
    return ir_path == nullptr || ir_path[0] == '\0'
        ? std::string()
        : std::string(ir_path) + ".cvsidx";
}

bool isSamePath(const std::string &left, const std::string &right)
{
    std::error_code error;
    const fs::path left_path = fs::weakly_canonical(left, error);
    if (error) {
        return left == right;
    }
    const fs::path right_path = fs::weakly_canonical(right, error);
    return !error && left_path == right_path;
}

bool consumesNext(const std::string &argument)
{
    return argument == "-o" || argument == "-MF" ||
        argument == "-MT" || argument == "-MQ" ||
        argument == "-MJ";
}

bool controlledFlag(const std::string &argument)
{
    return argument == "-c" || argument == "-MD" ||
        argument == "-MMD" || argument == "-MP" ||
        argument == "-MG" || argument == "-fPIC" ||
        argument == "-fpic" || argument == "-fPIE" ||
        argument == "-fpie" ||
        (argument.size() > 1U && argument[0] == '-' &&
         (argument[1] == 'O' || argument[1] == 'g'));
}

std::optional<fs::path> findCompilationDatabase(const fs::path &source)
{
    std::vector<fs::path> starts;
    starts.push_back(source.parent_path());
    std::error_code error;
    starts.push_back(fs::current_path(error));
    for (fs::path start : starts) {
        if (start.empty()) {
            continue;
        }
        for (;;) {
            if (fs::is_regular_file(start / "compile_commands.json", error)) {
                return start;
            }
            const fs::path parent = start.parent_path();
            if (parent == start || parent.empty()) {
                break;
            }
            start = parent;
        }
    }
    return std::nullopt;
}

std::vector<std::string> compileArguments(const std::string &source)
{
    std::vector<std::string> result;
    const fs::path absolute_source = fs::absolute(source).lexically_normal();
    const std::optional<fs::path> database_directory =
        findCompilationDatabase(absolute_source);
    if (!database_directory.has_value()) {
        result.emplace_back("-std=c11");
        return result;
    }

    CXCompilationDatabase_Error database_error =
        CXCompilationDatabase_NoError;
    CXCompilationDatabase database = clang_CompilationDatabase_fromDirectory(
        database_directory->string().c_str(), &database_error);
    if (database == nullptr ||
        database_error != CXCompilationDatabase_NoError) {
        result.emplace_back("-std=c11");
        return result;
    }
    CXCompileCommands commands = clang_CompilationDatabase_getCompileCommands(
        database, absolute_source.string().c_str());
    if (commands == nullptr || clang_CompileCommands_getSize(commands) == 0U) {
        if (commands != nullptr) {
            clang_CompileCommands_dispose(commands);
        }
        clang_CompilationDatabase_dispose(database);
        result.emplace_back("-std=c11");
        return result;
    }

    CXCompileCommand command = clang_CompileCommands_getCommand(commands, 0U);
    const std::string working_directory =
        takeString(clang_CompileCommand_getDirectory(command));
    if (!working_directory.empty()) {
        result.emplace_back("-working-directory");
        result.push_back(working_directory);
    }
    const unsigned argument_count = clang_CompileCommand_getNumArgs(command);
    bool skip_next = false;
    for (unsigned index = 1U; index < argument_count; ++index) {
        std::string argument =
            takeString(clang_CompileCommand_getArg(command, index));
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (consumesNext(argument)) {
            skip_next = true;
            continue;
        }
        if (controlledFlag(argument) ||
            argument.rfind("-o", 0U) == 0U ||
            argument.rfind("-MF", 0U) == 0U ||
            argument.rfind("-MT", 0U) == 0U ||
            argument.rfind("-MQ", 0U) == 0U ||
            isSamePath(argument, absolute_source.string())) {
            continue;
        }
        result.push_back(std::move(argument));
    }
    clang_CompileCommands_dispose(commands);
    clang_CompilationDatabase_dispose(database);
    if (result.empty()) {
        result.emplace_back("-std=c11");
    }
    return result;
}

bool replaceFile(const std::string &source, const std::string &destination)
{
    if (source.empty() || destination.empty()) {
        return false;
    }
    std::error_code error;
    fs::rename(source, destination, error);
    if (!error) {
        return true;
    }
    error.clear();
    fs::copy_file(
        source,
        destination,
        fs::copy_options::overwrite_existing,
        error);
    if (error) {
        return false;
    }
    error.clear();
    fs::remove(source, error);
    return true;
}

} // namespace

extern "C" int cvite_semantic_cache_stage(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path)
{
    if (source_path == nullptr || source_path[0] == '\0') {
        return -1;
    }
    const std::string active_index = indexPath(active_ir_path);
    const std::string staged_index = indexPath(staged_ir_path);
    if (staged_index.empty()) {
        return -1;
    }

    cvite::semantic::Index index;
    std::string error;
    if (!cvite::semantic::buildIndex(
            source_path,
            compileArguments(source_path),
            index,
            error) ||
        !cvite::semantic::writeIndex(index, staged_index, error)) {
        trace("could not stage '" + std::string(source_path) + "': " + error);
        std::error_code remove_error;
        fs::remove(staged_index, remove_error);
        return -1;
    }

    std::lock_guard<std::mutex> lock(cache_mutex);
    cache_entries[fs::absolute(source_path).lexically_normal().string()] = {
        active_index,
        staged_index,
    };
    return 0;
}

extern "C" void cvite_semantic_cache_promote(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path)
{
    if (source_path == nullptr || source_path[0] == '\0') {
        return;
    }
    const std::string source_key =
        fs::absolute(source_path).lexically_normal().string();
    const std::string active_index = indexPath(active_ir_path);
    const std::string staged_index = indexPath(staged_ir_path);
    if (active_index.empty() || staged_index.empty()) {
        return;
    }
    if (!replaceFile(staged_index, active_index)) {
        trace("could not promote semantic index for '" + source_key + "'");
        return;
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache_entries[source_key] = {active_index, staged_index};
}

extern "C" unsigned cvite_semantic_cache_report_pending(void)
{
    std::vector<std::pair<std::string, CacheEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        entries.assign(cache_entries.begin(), cache_entries.end());
    }

    unsigned report_count = 0U;
    for (const auto &entry : entries) {
        if (entry.second.active_index.empty() ||
            entry.second.staged_index.empty() ||
            !fs::is_regular_file(entry.second.active_index) ||
            !fs::is_regular_file(entry.second.staged_index)) {
            continue;
        }
        cvite::semantic::Index active;
        cvite::semantic::Index staged;
        std::string error;
        if (!cvite::semantic::readIndex(
                entry.second.active_index, active, error) ||
            !cvite::semantic::readIndex(
                entry.second.staged_index, staged, error)) {
            trace("could not read pending semantic indexes: " + error);
            continue;
        }
        const cvite::semantic::DiffSummary semantic_diff =
            cvite::semantic::diff(active, staged);
        if (!semantic_diff.changed) {
            continue;
        }
        if (report_count == 0U) {
            std::fprintf(
                stderr,
                "[cvite] semantic explanation for rejected native layout:\n");
        }
        std::fprintf(
            stderr,
            "source: %s\n%s",
            entry.first.c_str(),
            semantic_diff.text.c_str());
        ++report_count;
    }
    return report_count;
}

extern "C" void cvite_semantic_cache_clear(void)
{
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache_entries.clear();
}
