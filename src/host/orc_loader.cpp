#include "cvite/orc_loader.h"

#include "cvite/baseline.h"
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
#include <cinttypes>
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
constexpr const char *kRegisterFunctionSymbol = "__cvite_host_register_function";
constexpr const char *kRegisterStorageSymbol = "__cvite_host_register_storage";
constexpr const char *kTargetAtSymbol = "__cvite_host_target_at";
constexpr const char *kStorageForSymbol = "__cvite_host_storage_for";
constexpr const char *kStorageSymbolPrefix = "__cvite_storage.";
constexpr std::uint64_t kMaxManifestRecords = UINT64_C(1048576);

struct HostFunction final {
    std::string name;
    cvite_function_pointer address = nullptr;
};

struct Generation final {
    llvm::orc::JITDylib *dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources;
    std::vector<cvite_function_update> updates;
    bool baseline_prepared = false;
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

bool isPowerOfTwo(std::uint64_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
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

bool hasDuplicateId(const std::vector<cvite_id> &identities, cvite_id id)
{
    for (const cvite_id existing : identities) {
        if (cvite_id_equal(existing, id)) {
            return true;
        }
    }
    return false;
}

std::string storageSymbolName(cvite_id id)
{
    char name[64];
    const int length = std::snprintf(
        name,
        sizeof(name),
        "%s%016" PRIx64 "%016" PRIx64,
        kStorageSymbolPrefix,
        id.high,
        id.low);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(name)) {
        return std::string();
    }
    return std::string(name, static_cast<std::size_t>(length));
}

} // namespace

struct cvite_orc_loader {
    std::vector<llvm::orc::JITDylib *> automatic_link_dylibs;
    std::string automatic_target_triple;
    std::uint64_t next_automatic_link_id = 1U;
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
    created->automatic_target_triple =
        created->jit->getTargetTriple().str();
    created->host_functions.push_back(HostFunction{
        kTargetForSymbol,
        reinterpret_cast<cvite_function_pointer>(&__cvite_host_target_for),
    });
    created->host_functions.push_back(HostFunction{
        kRegisterFunctionSymbol,
        reinterpret_cast<cvite_function_pointer>(&__cvite_host_register_function),
    });
    created->host_functions.push_back(HostFunction{
        kRegisterStorageSymbol,
        reinterpret_cast<cvite_function_pointer>(&__cvite_host_register_storage),
    });
    created->host_functions.push_back(HostFunction{
        kTargetAtSymbol,
        reinterpret_cast<cvite_function_pointer>(&__cvite_host_target_at),
    });
    created->host_functions.push_back(HostFunction{
        kStorageForSymbol,
        reinterpret_cast<cvite_function_pointer>(&__cvite_host_storage_for),
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
    for (llvm::orc::JITDylib *native_dylib :
         loader->automatic_link_dylibs) {
        candidate_dylib.addToLinkOrder(
            *native_dylib,
            llvm::orc::JITDylibLookupFlags::MatchAllSymbols);
    }
    llvm::orc::SymbolMap host_symbols;
    for (const HostFunction &function : loader->host_functions) {
        host_symbols[loader->jit->mangleAndIntern(function.name)] = {
            llvm::orc::ExecutorAddr::fromPtr(function.address),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable,
        };
    }

    const std::size_t storage_count = cvite_host_storage_count();
    for (std::size_t index = 0U; index < storage_count; ++index) {
        cvite_host_storage storage;
        const cvite_status storage_status =
            cvite_host_storage_at(index, &storage, error);
        if (storage_status != CVITE_STATUS_OK) {
            return storage_status;
        }
        const std::string symbol_name = storageSymbolName(storage.id);
        if (symbol_name.empty()) {
            return fail(
                error,
                CVITE_STATUS_INVALID_STATE,
                "could not encode a persistent-storage symbol name");
        }
        host_symbols[loader->jit->mangleAndIntern(symbol_name)] = {
            llvm::orc::ExecutorAddr::fromPtr(storage.address),
            llvm::JITSymbolFlags::Exported,
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
        handle, Generation{&candidate_dylib, std::move(resources), {}, false});
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

extern "C" cvite_status cvite_orc_loader_prepare_baseline(
    cvite_orc_loader *loader,
    cvite_orc_generation generation,
    cvite_program_main *program_main,
    cvite_error *error)
{
    cvite_error_clear(error);
    if (loader == nullptr || generation == CVITE_ORC_GENERATION_INVALID ||
        program_main == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "invalid baseline preparation");
    }
    *program_main = nullptr;

    std::lock_guard<std::mutex> guard(loader->mutex);
    const auto found = loader->generations.find(generation);
    if (found == loader->generations.end()) {
        return fail(
            error,
            CVITE_STATUS_INVALID_ARGUMENT,
            "unknown baseline generation");
    }
    Generation &baseline = found->second;
    if (baseline.baseline_prepared) {
        return fail(
            error,
            CVITE_STATUS_INVALID_STATE,
            "baseline generation was already prepared");
    }

    auto manifest_symbol = loader->jit->lookup(
        *baseline.dylib, CVITE_BASELINE_MANIFEST_SYMBOL);
    if (!manifest_symbol) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            manifest_symbol.takeError(),
            "baseline object has no readable CVite manifest");
    }
    const cvite_baseline_manifest *manifest =
        manifest_symbol->toPtr<const cvite_baseline_manifest *>();
    if (manifest == nullptr ||
        manifest->schema != CVITE_BASELINE_MANIFEST_SCHEMA) {
        return fail(
            error,
            CVITE_STATUS_UNSUPPORTED_PROTOCOL,
            "baseline manifest schema is unsupported");
    }
    if (manifest->function_count > kMaxManifestRecords ||
        manifest->function_count > static_cast<std::uint64_t>(SIZE_MAX) ||
        manifest->storage_count > kMaxManifestRecords ||
        manifest->storage_count > static_cast<std::uint64_t>(SIZE_MAX)) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "baseline manifest record count is unreasonable");
    }
    if (manifest->function_count != 0U && manifest->functions == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "baseline manifest has no function records");
    }
    if (manifest->storage_count != 0U && manifest->storages == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "baseline manifest has no storage records");
    }

    std::vector<cvite_id> storage_identities;
    storage_identities.reserve(
        static_cast<std::size_t>(manifest->storage_count));
    for (std::uint64_t index = 0U;
         index < manifest->storage_count;
         ++index) {
        const cvite_baseline_storage &record = manifest->storages[index];
        const cvite_id id{record.id_high, record.id_low};
        const cvite_id layout{record.layout_high, record.layout_low};
        if (cvite_id_is_zero(id) || cvite_id_is_zero(layout) ||
            record.size == 0U || !isPowerOfTwo(record.alignment) ||
            record.size > static_cast<std::uint64_t>(SIZE_MAX) ||
            record.alignment > static_cast<std::uint64_t>(SIZE_MAX) ||
            record.address == nullptr || record.debug_name == nullptr ||
            record.debug_name[0] == '\0') {
            return fail(
                error,
                CVITE_STATUS_INVALID_PACKET,
                "baseline manifest contains an invalid storage record");
        }
        if (hasDuplicateId(storage_identities, id)) {
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "baseline manifest contains a duplicate storage ID");
        }
        storage_identities.push_back(id);
    }

    std::vector<cvite_id> function_identities;
    function_identities.reserve(
        static_cast<std::size_t>(manifest->function_count));
    for (std::uint64_t index = 0U;
         index < manifest->function_count;
         ++index) {
        const cvite_baseline_function &record = manifest->functions[index];
        const cvite_id id{record.id_high, record.id_low};
        const cvite_id abi{record.abi_high, record.abi_low};
        if (cvite_id_is_zero(id) || cvite_id_is_zero(abi) ||
            record.initial_target == nullptr || record.slot == nullptr ||
            record.debug_name == nullptr || record.debug_name[0] == '\0') {
            return fail(
                error,
                CVITE_STATUS_INVALID_PACKET,
                "baseline manifest contains an invalid function record");
        }
        if (hasDuplicateId(function_identities, id)) {
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "baseline manifest contains a duplicate function ID");
        }
        function_identities.push_back(id);
    }

    for (std::uint64_t index = 0U;
         index < manifest->storage_count;
         ++index) {
        const cvite_baseline_storage &record = manifest->storages[index];
        const cvite_storage_definition definition = {
            cvite_id{record.id_high, record.id_low},
            cvite_id{record.layout_high, record.layout_low},
            static_cast<std::size_t>(record.size),
            static_cast<std::size_t>(record.alignment),
            nullptr,
            record.debug_name,
        };
        cvite_host_storage registered;
        const cvite_status register_status = cvite_host_register_storage(
            &definition, record.address, &registered, error);
        if (register_status != CVITE_STATUS_OK) {
            return register_status;
        }
    }

    for (std::uint64_t index = 0U;
         index < manifest->function_count;
         ++index) {
        const cvite_baseline_function &record = manifest->functions[index];
        const cvite_function_definition definition = {
            cvite_id{record.id_high, record.id_low},
            cvite_id{record.abi_high, record.abi_low},
            record.initial_target,
            record.debug_name,
        };
        cvite_host_function registered;
        const cvite_status register_status = cvite_host_register_function(
            &definition, &registered, error);
        if (register_status != CVITE_STATUS_OK) {
            return register_status;
        }
        if (registered.slot > static_cast<std::size_t>(UINT64_MAX)) {
            return fail(
                error,
                CVITE_STATUS_INVALID_STATE,
                "baseline dispatch slot does not fit the compiler ABI");
        }
        *record.slot = static_cast<std::uint64_t>(registered.slot);
    }

    auto main_symbol = loader->jit->lookup(
        *baseline.dylib, CVITE_PROGRAM_MAIN_SYMBOL);
    if (!main_symbol) {
        return fail(
            error,
            CVITE_STATUS_UNKNOWN_SYMBOL,
            main_symbol.takeError(),
            "baseline has no supported C main entry point");
    }

    *program_main = main_symbol->toPtr<int(int, char **)>();
    baseline.baseline_prepared = true;
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
    if (manifest->function_count > kMaxManifestRecords ||
        manifest->function_count > static_cast<std::uint64_t>(SIZE_MAX) ||
        manifest->storage_count > kMaxManifestRecords ||
        manifest->storage_count > static_cast<std::uint64_t>(SIZE_MAX)) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "candidate manifest record count is unreasonable");
    }
    if (manifest->functions == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "candidate manifest has no function records");
    }
    if (manifest->storage_count != 0U && manifest->storages == nullptr) {
        return fail(
            error,
            CVITE_STATUS_INVALID_PACKET,
            "candidate manifest has no storage records");
    }

    std::vector<cvite_id> storage_identities;
    storage_identities.reserve(
        static_cast<std::size_t>(manifest->storage_count));
    for (std::uint64_t index = 0U;
         index < manifest->storage_count;
         ++index) {
        const cvite_candidate_storage &record = manifest->storages[index];
        const cvite_id id{record.id_high, record.id_low};
        const cvite_id layout{record.layout_high, record.layout_low};
        if (cvite_id_is_zero(id) || cvite_id_is_zero(layout) ||
            record.size == 0U || !isPowerOfTwo(record.alignment) ||
            record.size > static_cast<std::uint64_t>(SIZE_MAX) ||
            record.alignment > static_cast<std::uint64_t>(SIZE_MAX) ||
            record.debug_name == nullptr || record.debug_name[0] == '\0') {
            return fail(
                error,
                CVITE_STATUS_INVALID_PACKET,
                "candidate manifest contains an invalid storage record");
        }
        if (hasDuplicateId(storage_identities, id)) {
            return fail(
                error,
                CVITE_STATUS_DUPLICATE_SYMBOL,
                "candidate manifest contains a duplicate storage ID");
        }
        storage_identities.push_back(id);

        const cvite_storage_definition definition = {
            id,
            layout,
            static_cast<std::size_t>(record.size),
            static_cast<std::size_t>(record.alignment),
            nullptr,
            record.debug_name,
        };
        cvite_host_storage storage;
        const cvite_status storage_status = cvite_host_require_storage(
            &definition, &storage, error);
        if (storage_status != CVITE_STATUS_OK) {
            return storage_status;
        }
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
