#include "baseline_manifest_pass.h"
#include "transform_support.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdint>
#include <string>
#include <utility>

namespace {

using cvite::transform::Hash128;

constexpr llvm::StringLiteral kPassName = "cvite-baseline-manifest";
constexpr llvm::StringLiteral kBaselineFlag = "cvite.baseline.schema";
constexpr llvm::StringLiteral kManifestFlag = "cvite.baseline.manifest.schema";
constexpr llvm::StringLiteral kManifestSymbol = "__cvite_baseline_manifest";
constexpr llvm::StringLiteral kFunctionRecordsSymbol =
    "__cvite_baseline_records";
constexpr llvm::StringLiteral kStorageRecordsSymbol =
    "__cvite_baseline_storage_records";
constexpr llvm::StringLiteral kProgramMainSymbol = "__cvite_program_main";
constexpr unsigned kManifestSchema = 3U;

struct BaselineFunction final {
    llvm::Function *implementation = nullptr;
    llvm::GlobalVariable *slot = nullptr;
    Hash128 identity;
    Hash128 abi;
    Hash128 implementation_fingerprint;
    std::string debug_name;
};

struct BaselineStorage final {
    llvm::GlobalVariable *global = nullptr;
    Hash128 identity;
    Hash128 layout;
    std::uint64_t size = 0U;
    std::uint64_t alignment = 0U;
    std::string debug_name;
};

bool parseHash(llvm::StringRef hex, Hash128 &hash)
{
    if (hex.size() != 32U) {
        return false;
    }

    /*
     * MD5::stringifyResult prints the digest bytes in array order, while
     * MD5Result::words() interprets each 64-bit half as little-endian. Function
     * identities are created from words(), so reconstruct those exact numeric
     * values instead of treating the two printed halves as big-endian numbers.
     */
    std::uint64_t words[2] = {0U, 0U};
    for (unsigned index = 0U; index < 16U; ++index) {
        unsigned byte = 0U;
        if (hex.substr(index * 2U, 2U).getAsInteger(16U, byte) ||
            byte > 0xffU) {
            return false;
        }
        words[index / 8U] |=
            static_cast<std::uint64_t>(byte) << ((index % 8U) * 8U);
    }

    hash = Hash128{words[1], words[0], hex.str()};
    return true;
}

bool extractImplementationFingerprint(
    llvm::Module &module,
    llvm::Function &implementation,
    Hash128 &fingerprint)
{
    llvm::MDNode *metadata = implementation.getMetadata(
        cvite::transform::kImplementationMetadata);
    if (metadata == nullptr || metadata->getNumOperands() < 4U) {
        module.getContext().emitError(
            "CVite implementation is missing its change fingerprint");
        return false;
    }

    const auto *schema = llvm::dyn_cast<llvm::MDString>(
        metadata->getOperand(1U).get());
    const auto *high_metadata = llvm::dyn_cast<llvm::ConstantAsMetadata>(
        metadata->getOperand(2U).get());
    const auto *low_metadata = llvm::dyn_cast<llvm::ConstantAsMetadata>(
        metadata->getOperand(3U).get());
    const auto *high = high_metadata == nullptr
        ? nullptr
        : llvm::dyn_cast<llvm::ConstantInt>(high_metadata->getValue());
    const auto *low = low_metadata == nullptr
        ? nullptr
        : llvm::dyn_cast<llvm::ConstantInt>(low_metadata->getValue());
    if (schema == nullptr ||
        schema->getString() != cvite::transform::kImplementationSchema ||
        high == nullptr || low == nullptr) {
        module.getContext().emitError(
            "CVite implementation has a malformed change fingerprint");
        return false;
    }

    fingerprint = Hash128{
        high->getZExtValue(),
        low->getZExtValue(),
        std::string(),
    };
    return true;
}

bool extractFunction(
    llvm::Module &module,
    llvm::Function &entry,
    BaselineFunction &result)
{
    if (entry.isDeclaration() || entry.getName().starts_with("__cvite_")) {
        return false;
    }

    llvm::MDNode *metadata =
        entry.getMetadata(cvite::transform::kFunctionMetadata);
    if (metadata == nullptr || metadata->getNumOperands() < 5U) {
        return false;
    }

    const auto *abi_high_metadata = llvm::dyn_cast<llvm::ConstantAsMetadata>(
        metadata->getOperand(2U).get());
    const auto *abi_low_metadata = llvm::dyn_cast<llvm::ConstantAsMetadata>(
        metadata->getOperand(3U).get());
    const auto *identity_metadata = llvm::dyn_cast<llvm::MDString>(
        metadata->getOperand(4U).get());
    if (abi_high_metadata == nullptr || abi_low_metadata == nullptr ||
        identity_metadata == nullptr) {
        module.getContext().emitError(
            "CVite stable entry has malformed lowered-ABI metadata");
        return false;
    }

    const auto *abi_high = llvm::dyn_cast<llvm::ConstantInt>(
        abi_high_metadata->getValue());
    const auto *abi_low = llvm::dyn_cast<llvm::ConstantInt>(
        abi_low_metadata->getValue());
    Hash128 identity;
    if (abi_high == nullptr || abi_low == nullptr ||
        !parseHash(identity_metadata->getString(), identity)) {
        module.getContext().emitError(
            "CVite stable entry has an invalid function identity");
        return false;
    }

    const std::string implementation_name =
        "__cvite_impl." + identity.hex;
    const std::string slot_name = "__cvite_slot." + identity.hex;
    llvm::Function *implementation = module.getFunction(implementation_name);
    llvm::GlobalVariable *slot = module.getGlobalVariable(slot_name, true);
    if (implementation == nullptr || slot == nullptr) {
        module.getContext().emitError(
            "CVite stable entry is missing its implementation or dispatch slot");
        return false;
    }

    Hash128 implementation_fingerprint;
    if (!extractImplementationFingerprint(
            module, *implementation, implementation_fingerprint)) {
        return false;
    }

    result = BaselineFunction{
        implementation,
        slot,
        identity,
        Hash128{
            abi_high->getZExtValue(),
            abi_low->getZExtValue(),
            std::string(),
        },
        implementation_fingerprint,
        entry.getName().str(),
    };
    return true;
}

bool extractStorage(
    llvm::Module &module,
    llvm::GlobalVariable &global,
    BaselineStorage &result)
{
    if (!cvite::transform::shouldTrackStorageDefinition(global)) {
        return false;
    }

    const std::uint64_t size =
        cvite::transform::storageSize(module, global);
    const std::uint64_t alignment =
        cvite::transform::storageAlignment(module, global);
    if (size == 0U || alignment == 0U) {
        module.getContext().emitError(
            "CVite persistent storage has an unsupported target layout");
        return false;
    }

    const std::string debug_name = global.getName().str();
    result = BaselineStorage{
        &global,
        cvite::transform::hash128(cvite::transform::storageIdentitySeed(
            module, global, debug_name)),
        cvite::transform::hash128(
            cvite::transform::storageLayoutSeed(module, global)),
        size,
        alignment,
        debug_name,
    };
    return true;
}

llvm::GlobalVariable *createDebugName(
    llvm::Module &module,
    llvm::StringRef prefix,
    llvm::StringRef name,
    const Hash128 &identity)
{
    llvm::Constant *data = llvm::ConstantDataArray::getString(
        module.getContext(), name, true);
    auto *global = new llvm::GlobalVariable(
        module,
        data->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        data,
        prefix.str() + identity.hex);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(1U));
    return global;
}

llvm::GlobalVariable *createFunctionRecords(
    llvm::Module &module,
    llvm::ArrayRef<BaselineFunction> functions)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *record_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, i64, i64, pointer, pointer, pointer},
        false);

    llvm::SmallVector<llvm::Constant *, 32> records;
    records.reserve(functions.size());
    for (const BaselineFunction &function : functions) {
        llvm::GlobalVariable *debug_name = createDebugName(
            module,
            "__cvite_baseline_name.",
            function.debug_name,
            function.identity);
        records.push_back(llvm::ConstantStruct::get(
            record_type,
            {
                llvm::ConstantInt::get(i64, function.identity.high),
                llvm::ConstantInt::get(i64, function.identity.low),
                llvm::ConstantInt::get(i64, function.abi.high),
                llvm::ConstantInt::get(i64, function.abi.low),
                llvm::ConstantInt::get(
                    i64, function.implementation_fingerprint.high),
                llvm::ConstantInt::get(
                    i64, function.implementation_fingerprint.low),
                function.implementation,
                function.slot,
                debug_name,
            }));
    }

    llvm::ArrayType *records_type = llvm::ArrayType::get(
        record_type, static_cast<std::uint64_t>(records.size()));
    auto *records_global = new llvm::GlobalVariable(
        module,
        records_type,
        true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(records_type, records),
        kFunctionRecordsSymbol);
    records_global->setAlignment(llvm::Align(8U));
    return records_global;
}

llvm::GlobalVariable *createStorageRecords(
    llvm::Module &module,
    llvm::ArrayRef<BaselineStorage> storages)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *record_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, i64, i64, pointer, pointer},
        false);

    llvm::SmallVector<llvm::Constant *, 32> records;
    records.reserve(storages.size());
    for (const BaselineStorage &storage : storages) {
        llvm::GlobalVariable *debug_name = createDebugName(
            module,
            "__cvite_baseline_storage_name.",
            storage.debug_name,
            storage.identity);
        records.push_back(llvm::ConstantStruct::get(
            record_type,
            {
                llvm::ConstantInt::get(i64, storage.identity.high),
                llvm::ConstantInt::get(i64, storage.identity.low),
                llvm::ConstantInt::get(i64, storage.layout.high),
                llvm::ConstantInt::get(i64, storage.layout.low),
                llvm::ConstantInt::get(i64, storage.size),
                llvm::ConstantInt::get(i64, storage.alignment),
                storage.global,
                debug_name,
            }));
    }

    llvm::ArrayType *records_type = llvm::ArrayType::get(
        record_type, static_cast<std::uint64_t>(records.size()));
    auto *records_global = new llvm::GlobalVariable(
        module,
        records_type,
        true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(records_type, records),
        kStorageRecordsSymbol);
    records_global->setAlignment(llvm::Align(8U));
    return records_global;
}

void createManifest(
    llvm::Module &module,
    llvm::ArrayRef<BaselineFunction> functions,
    llvm::ArrayRef<BaselineStorage> storages)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::GlobalVariable *function_records =
        createFunctionRecords(module, functions);
    llvm::GlobalVariable *storage_records =
        createStorageRecords(module, storages);

    llvm::StructType *manifest_type = llvm::StructType::get(
        context, {i64, i64, pointer, i64, pointer}, false);
    auto *manifest = new llvm::GlobalVariable(
        module,
        manifest_type,
        true,
        llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantStruct::get(
            manifest_type,
            {
                llvm::ConstantInt::get(i64, kManifestSchema),
                llvm::ConstantInt::get(
                    i64, static_cast<std::uint64_t>(functions.size())),
                function_records,
                llvm::ConstantInt::get(
                    i64, static_cast<std::uint64_t>(storages.size())),
                storage_records,
            }),
        kManifestSymbol);
    manifest->setVisibility(llvm::GlobalValue::DefaultVisibility);
    manifest->setDSOLocal(true);
    manifest->setAlignment(llvm::Align(8U));

    llvm::SmallVector<llvm::GlobalValue *, 1> retained;
    retained.push_back(manifest);
    llvm::appendToCompilerUsed(module, retained);
}

void createProgramMain(llvm::Module &module)
{
    llvm::Function *main = module.getFunction("main");
    if (main == nullptr || main->isDeclaration()) {
        return;
    }

    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i32 = llvm::Type::getInt32Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    if (main->getReturnType() != i32 ||
        (main->arg_size() != 0U && main->arg_size() != 2U)) {
        context.emitError(
            "CVite currently supports int main(void) and int main(int, char **)");
        return;
    }
    if (main->arg_size() == 2U) {
        auto argument = main->arg_begin();
        llvm::Type *argc_type = argument->getType();
        ++argument;
        llvm::Type *argv_type = argument->getType();
        if (argc_type != i32 || argv_type != pointer) {
            context.emitError(
                "CVite main arguments do not match the target C ABI");
            return;
        }
    }

    llvm::FunctionType *canonical_type = llvm::FunctionType::get(
        i32, {i32, pointer}, false);
    llvm::Function *wrapper = llvm::Function::Create(
        canonical_type,
        llvm::GlobalValue::ExternalLinkage,
        kProgramMainSymbol,
        &module);
    wrapper->setVisibility(llvm::GlobalValue::DefaultVisibility);
    wrapper->setDSOLocal(true);
    wrapper->addFnAttr(llvm::Attribute::NoInline);

    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", wrapper);
    llvm::IRBuilder<> builder(entry);
    llvm::CallInst *result = nullptr;
    if (main->arg_size() == 0U) {
        result = builder.CreateCall(main);
    } else {
        llvm::SmallVector<llvm::Value *, 2> arguments;
        auto wrapper_argument = wrapper->arg_begin();
        arguments.push_back(&*wrapper_argument++);
        arguments.push_back(&*wrapper_argument);
        result = builder.CreateCall(main, arguments);
    }
    result->setCallingConv(main->getCallingConv());
    builder.CreateRet(result);
}

class CViteBaselineManifestPass final
    : public llvm::PassInfoMixin<CViteBaselineManifestPass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        if (module.getModuleFlag(kManifestFlag) != nullptr) {
            return llvm::PreservedAnalyses::all();
        }
        if (module.getModuleFlag(kBaselineFlag) == nullptr) {
            module.getContext().emitError(
                "cvite-baseline-manifest must run after cvite-baseline");
            return llvm::PreservedAnalyses::all();
        }

        llvm::SmallVector<BaselineFunction, 32> functions;
        for (llvm::Function &function : module) {
            BaselineFunction record;
            if (extractFunction(module, function, record)) {
                functions.push_back(std::move(record));
            }
        }

        llvm::SmallVector<BaselineStorage, 32> storages;
        for (llvm::GlobalVariable &global : module.globals()) {
            BaselineStorage record;
            if (extractStorage(module, global, record)) {
                storages.push_back(std::move(record));
            }
        }

        createManifest(module, functions, storages);
        createProgramMain(module);
        module.addModuleFlag(
            llvm::Module::Error,
            kManifestFlag,
            kManifestSchema);
        return llvm::PreservedAnalyses::none();
    }

    static bool isRequired() { return true; }
};

} // namespace

void cviteRegisterBaselineManifestPass(llvm::PassBuilder &builder)
{
    builder.registerPipelineParsingCallback(
        [](llvm::StringRef name,
           llvm::ModulePassManager &manager,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != kPassName) {
                return false;
            }
            manager.addPass(CViteBaselineManifestPass());
            return true;
        });
}
