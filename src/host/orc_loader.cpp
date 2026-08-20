#include "cvite/orc_loader.h"

#include "cvite/candidate.h"
#include "cvite/host.h"

#include "llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr const char *kTargetForSymbol = "__cvite_host_target_for";
constexpr std::uint64_t kMaxCandidateFunctions = UINT64_C(1048576);

struct HostFunction final {
    std::string name;
    cvite_function_pointer address = nullptr;
};

struct Generation final {
    llvm::orc::JITDylib *dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources;
    std::vector<cvite_function_update> updates;
};

struct NativeTargetState final {
    bool failed = false;
    std::string message;
};

NativeTargetState &nativeTargetState()
{
    static NativeTargetState state;
    static std::once_flag once;

    std::call_once(once, [] {
        NativeTargetState &target = state;
        if (llvm::InitializeNativeTarget()) {
            target.failed = true;
            target.message = "LLVM could not initialize the native target";
            return;
        }
        if (llvm::InitializeNativeTargetAsmPrinter()) {
            target.failed = true;
            target.message = "LLVM could not initialize the native assembly printer";
        }
    });

    return state;
}

cvite_status fail(
    cvite_error *error,
    cvite_status status,
    const std::string &message)
{
    cvite_error_clear(error);
    if (error != nullptr) {
        error->status = status;
        (void)std::snprintf(
            error->message,
            sizeof(error->message),
            "%s",
            message.c_str());
    }
    return status;
}

cvite_status fail(
    cvite_error *error,
    cvite_status status,
    llvm::Error llvm_error,
    const char *prefix)
{
    std::string message(prefix);
    message += ": ";
    message += llvm::toString(std::move(llvm_error));
    return fail(error, status, message);
}

llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> createJITLinkLayer(
    llvm::orc::ExecutionSession &session,
    const llvm::Triple &)
{
    auto memory_manager = llvm::jitlink::InProcessMemoryManager::Create();
    if (!memory_manager) {
        return memory_manager.takeError();
    }

    return std::unique_ptr<llvm::orc::ObjectLayer>(
        std::make_unique<llvm::orc::ObjectLinkingLayer>(
            session,
            std::move(*memory_manager)));
}

bool hasDuplicateUpdate(
    const std::vector<cvite_function_update> &updates,
    cvite_id id)
{
    for (const cvite_function_update &update : updates) {
        if (cvite_id_equal(update.id, id)) {
            return true;
        }
    }
    return false;
}

} // namespace

struct cvite_orc_loader {
    std::mutex mutex;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::vector<HostFunction> host_functions;
    std::unordered_map<cvite_orc_generation, Generation> generations;
    cvite_orc_generation next_generation = 1U;
};

extern "C" cvite_status cvite_orc_loader_create(
    cvite_orc_loader **loader,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "loader output is null");
    }
    *loader = nullptr;

    NativeTargetState &target = nativeTargetState();
    if (target.failed) {
        return fail(error, CVITE_STATUS_INVALID_STATE, target.message);
    }

    auto jit = llvm::orc::LLJITBuilder()
                   .setObjectLinkingLayerCreator(createJITLinkLayer)
                   .create();
    if (!jit) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            jit.takeError(),
            "could not create ORC/JITLink session");
    }

    auto created = std::unique_ptr<cvite_orc_loader>(
        new (std::nothrow) cvite_orc_loader());
    if (!created) {
        return fail(error, CVITE_STATUS_OUT_OF_MEMORY, "could not allocate ORC loader");
    }

    created->jit = std::move(*jit);
    created->host_functions.push_back(HostFunction{
        kTargetForSymbol,
        reinterpret_cast<cvite_function_pointer>(&__cvite_host_target_for),
    });
    *loader = created.release();
    return CVITE_STATUS_OK;
}

extern "C" void cvite_orc_loader_destroy(cvite_orc_loader *loader)
{
    if (loader == nullptr) {
        return;
    }

    for (auto &entry : loader->generations) {
        if (entry.second.resources) {
            llvm::consumeError(entry.second.resources->remove());
        }
    }
    delete loader;
}

extern "C" cvite_status cvite_orc_loader_define_function(
    cvite_orc_loader *loader,
    const char *name,
    cvite_function_pointer address,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || name == nullptr || name[0] == '\0' ||
        address == nullptr) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "invalid host function");
    }

    std::lock_guard<std::mutex> guard(loader->mutex);
    if (!loader->generations.empty()) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "host functions must be defined before staging generations");
    }

    for (const HostFunction &function : loader->host_functions) {
        if (function.name == name) {
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "host function is already defined: " + function.name);
        }
    }

    loader->host_functions.push_back(HostFunction{name, address});
    return CVITE_STATUS_OK;
}

extern "C" cvite_status cvite_orc_loader_stage_object(
    cvite_orc_loader *loader,
    const char *object_path,
    cvite_orc_generation *generation,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || object_path == nullptr || object_path[0] == '\0' ||
        generation == nullptr) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "invalid object stage request");
    }
    *generation = CVITE_ORC_GENERATION_INVALID;

    std::lock_guard<std::mutex> guard(loader->mutex);
    auto object = llvm::MemoryBuffer::getFile(object_path, false, false);
    if (!object) {
        return fail(
            error,
            CVITE_STATUS_IO_ERROR,
            "could not read object file '" + std::string(object_path) +
                "': " + object.getError().message());
    }

    const cvite_orc_generation handle = loader->next_generation++;
    const std::string dylib_name = "cvite.generation." + std::to_string(handle);
    auto dylib = loader->jit->createJITDylib(dylib_name);
    if (!dylib) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            dylib.takeError(),
            "could not create candidate namespace");
    }

    llvm::orc::JITDylib &candidate_dylib = *dylib;
    llvm::orc::SymbolMap host_symbols;
    for (const HostFunction &function : loader->host_functions) {
        host_symbols[loader->jit->mangleAndIntern(function.name)] = {
            llvm::orc::ExecutorAddr::fromPtr(function.address),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable,
        };
    }
    if (!host_symbols.empty()) {
        if (llvm::Error define_error =
                candidate_dylib.define(
                    llvm::orc::absoluteSymbols(std::move(host_symbols)))) {
            return fail(
                error,
                CVITE_STATUS_LINK_ERROR,
                std::move(define_error),
                "could not expose host symbols to candidate");
        }
    }

    llvm::orc::ResourceTrackerSP resources =
        candidate_dylib.createResourceTracker();
    if (llvm::Error add_error = loader->jit->addObjectFile(
            resources,
            std::move(*object))) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            std::move(add_error),
            "could not stage candidate object");
    }

    loader->generations.emplace(
        handle, Generation{&candidate_dylib, std::move(resources), {}});
    *generation = handle;
    return CVITE_STATUS_OK;
}

extern "C" cvite_status cvite_orc_loader_lookup_function(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    const char *symbol_name,
    cvite_function_pointer *address,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || generation == CVITE_ORC_GENERATION_INVALID ||
        symbol_name == nullptr || symbol_name[0] == '\0' || address == nullptr) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "invalid symbol lookup");
    }
    *address = nullptr;

    std::lock_guard<std::mutex> guard(loader->mutex);
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "unknown candidate generation");
    }

    auto symbol = loader->jit->lookup(*found->second.dylib, symbol_name);
    if (!symbol) {
        return fail(
            error,
            CVITE_STATUS_LINK_ERROR,
            symbol.takeError(),
            "could not resolve candidate function");
    }

    *address = symbol->toPtr<void()>();
    return CVITE_STATUS_OK;
}

extern "C" cvite_status cvite_orc_loader_prepare_patch(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    uint64_t expected_generation,
    uint64_t candidate_generation,
    cvite_patch *patch,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || generation == CVITE_ORC_GENERATION_INVALID ||
        patch == nullptr) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "invalid patch preparation");
    }
    *patch = cvite_patch{};

    std::lock_guard<std::mutex> guard(loader->mutex);
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "unknown candidate generation");
    }

    auto symbol = loader->jit->lookup(
        *found->second.dylib, CVITE_CANDIDATE_MANIFEST_SYMBOL);
    if (!symbol) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            symbol.takeError(),
            "candidate object has no readable CVite manifest");
    }

    const cvite_candidate_manifest *manifest =
        symbol->toPtr<const cvite_candidate_manifest *>();
    if (manifest == nullptr ||
        manifest->schema != CVITE_CANDIDATE_MANIFEST_SCHEMA) {
        return fail(
            error,
            CVITE_STATUS_UNSUPPORTED_PROTOCOL,
            "candidate manifest schema is unsupported");
    }
    if (manifest->function_count == 0U) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "candidate manifest contains no refreshable functions");
    }
    if (manifest->function_count > kMaxCandidateFunctions ||
        manifest->function_count > (std::uint64_t)SIZE_MAX) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "candidate manifest function count is unreasonable");
    }
    if (manifest->functions == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "candidate manifest has no function records");
    }

    Generation &candidate = found->second;
    candidate.updates.clear();
    candidate.updates.reserve(
        static_cast<std::size_t>(manifest->function_count));
    for (std::uint64_t index = 0U;
         index < manifest->function_count;
         ++index) {
        const cvite_candidate_function &record = manifest->functions[index];
        const cvite_id id{record.id_high, record.id_low};
        const cvite_id abi{record.abi_high, record.abi_low};

        if (cvite_id_is_zero(id) || cvite_id_is_zero(abi) ||
            record.target == nullptr || record.debug_name == nullptr ||
            record.debug_name[0] == '\0') {
            candidate.updates.clear();
            return fail(
                error,
                CVITE_STATUS_INVALID_PACKET,
                "candidate manifest contains an invalid function record");
        }
        if (hasDuplicateUpdate(candidate.updates, id)) {
            candidate.updates.clear();
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "candidate manifest contains a duplicate function ID");
        }

        candidate.updates.push_back(cvite_function_update{
            id,
            abi,
            record.target,
        });
    }

    patch->expected_generation = expected_generation;
    patch->candidate_generation = candidate_generation;
    patch->functions = candidate.updates.data();
    patch->function_count = candidate.updates.size();
    return CVITE_STATUS_OK;
}

extern "C" cvite_status cvite_orc_loader_discard_generation(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || generation == CVITE_ORC_GENERATION_INVALID) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "invalid generation discard");
    }

    std::lock_guard<std::mutex> guard(loader->mutex);
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(error, CVITE_STATUS_INVALID_ARGUMENT, "unknown candidate generation");
    }

    if (found->second.resources) {
        if (llvm::Error remove_error = found->second.resources->remove()) {
            return fail(
                error,
                CVITE_STATUS_LINK_ERROR,
                std::move(remove_error),
                "could not discard candidate generation");
        }
    }
    loader->generations.erase(found);
    return CVITE_STATUS_OK;
}
