#include "candidate_pass.h"
#include "cvite/candidate.h"
#include "transform_support.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Comdat.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdint>
#include <string>

namespace {

using cvite::transform::Hash128;

constexpr llvm::StringLiteral kPassName = "cvite-candidate";
constexpr llvm::StringLiteral kCandidateFlag = "cvite.candidate.schema";
constexpr llvm::StringLiteral kBaselineFlag = "cvite.baseline.schema";
constexpr llvm::StringLiteral kTargetFor = "__cvite_host_target_for";
constexpr llvm::StringLiteral kCallEnter = "__cvite_host_call_enter";
constexpr llvm::StringLiteral kCallLeave = "__cvite_host_call_leave";
constexpr llvm::StringLiteral kManifestSymbol = "__cvite_candidate_manifest";
constexpr llvm::StringLiteral kFunctionRecordsSymbol =
    "__cvite_candidate_records";
constexpr llvm::StringLiteral kStorageRecordsSymbol =
    "__cvite_candidate_storage_records";
constexpr unsigned kCandidateSchema = 3U;

struct CandidateFunction final {
    llvm::Function *implementation = nullptr;
    llvm::Function *entry = nullptr;
    Hash128 identity;
    Hash128 abi;
    Hash128 implementation_fingerprint;
    bool entry_address_escapes = false;
    std::string debug_name;
};

struct CandidateStorage final {
    llvm::GlobalVariable *proxy = nullptr;
    Hash128 identity;
    Hash128 layout;
    std::uint64_t size = 0U;
    std::uint64_t alignment = 0U;
    std::string debug_name;
};

bool hasBlockAddressUse(const llvm::Function &function)
{
    for (const llvm::User *user : function.users()) {
        if (llvm::isa<llvm::BlockAddress>(user)) {
            return true;
        }
    }
    return false;
}

bool isDirectCallUse(
    const llvm::User &user,
    const llvm::Function &function)
{
    const auto *call = llvm::dyn_cast<llvm::CallBase>(&user);
    return call != nullptr &&
        call->getCalledOperand()->stripPointerCasts() == &function;
}

bool hasAddressEscape(const llvm::Function &function)
{
    for (const llvm::User *user : function.users()) {
        if (user == nullptr || !isDirectCallUse(*user, function)) {
            return true;
        }
    }
    return false;
}

bool hasSupportedLinkage(const llvm::Function &function)
{
    return function.hasExternalLinkage() || function.hasInternalLinkage() ||
        function.hasPrivateLinkage();
}

bool shouldTransform(const llvm::Function &function)
{
    return cvite::transform::shouldIndex(function) &&
        function.getName() != "main" &&
        !function.getFunctionType()->isVarArg() &&
        hasSupportedLinkage(function) &&
        !hasBlockAddressUse(function) &&
        function.getAddressSpace() == 0U;
}

llvm::AttributeList abiAttributes(const llvm::Function &function)
{
    llvm::SmallVector<llvm::AttributeSet, 8> argument_attributes;
    const llvm::AttributeList source = function.getAttributes();

    argument_attributes.reserve(function.arg_size());
    for (unsigned index = 0U; index < function.arg_size(); ++index) {
        argument_attributes.push_back(source.getParamAttrs(index));
    }

    return llvm::AttributeList::get(
        function.getContext(),
        llvm::AttributeSet(),
        source.getRetAttrs(),
        argument_attributes);
}

llvm::FunctionCallee getCallScopeFunction(
    llvm::Module &module,
    llvm::StringRef name)
{
    llvm::FunctionType *type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(module.getContext()), false);
    return module.getOrInsertFunction(name, type);
}

llvm::FunctionCallee getTargetFunction(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::FunctionType *type = llvm::FunctionType::get(
        pointer,
        {i64, i64, i64, i64},
        false);
    return module.getOrInsertFunction(kTargetFor, type);
}

void copyArgumentNames(llvm::Function &destination, const llvm::Function &source)
{
    auto destination_argument = destination.arg_begin();
    for (const llvm::Argument &source_argument : source.args()) {
        destination_argument->setName(source_argument.getName());
        ++destination_argument;
    }
}

void setRefreshMetadata(
    llvm::Function &function,
    const Hash128 &identity,
    const Hash128 &abi)
{
    llvm::LLVMContext &context = function.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Metadata *metadata[] = {
        llvm::MDString::get(context, abi.hex),
        llvm::MDString::get(context, cvite::transform::kAbiSchema),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, abi.high)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, abi.low)),
        llvm::MDString::get(context, identity.hex),
    };
    function.setMetadata(
        cvite::transform::kFunctionMetadata,
        llvm::MDNode::get(context, metadata));
}

void setStorageMetadata(
    llvm::GlobalVariable &global,
    const CandidateStorage &storage)
{
    llvm::LLVMContext &context = global.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Metadata *metadata[] = {
        llvm::MDString::get(context, storage.identity.hex),
        llvm::MDString::get(context, storage.layout.hex),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, storage.identity.high)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, storage.identity.low)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, storage.layout.high)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, storage.layout.low)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, storage.size)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, storage.alignment)),
    };
    global.setMetadata(
        cvite::transform::kStorageMetadata,
        llvm::MDNode::get(context, metadata));
}

CandidateStorage transformStorage(
    llvm::Module &module,
    llvm::GlobalVariable &storage)
{
    const std::string original_name = storage.getName().str();
    const Hash128 identity = cvite::transform::hash128(
        cvite::transform::storageIdentitySeed(
            module, storage, original_name));
    const Hash128 layout = cvite::transform::hash128(
        cvite::transform::storageLayoutSeed(module, storage));
    const std::uint64_t size =
        cvite::transform::storageSize(module, storage);
    const std::uint64_t alignment =
        cvite::transform::storageAlignment(module, storage);

    auto *proxy = new llvm::GlobalVariable(
        module,
        storage.getValueType(),
        false,
        llvm::GlobalValue::ExternalLinkage,
        nullptr,
        cvite::transform::storageSymbolName(identity));
    proxy->setVisibility(llvm::GlobalValue::DefaultVisibility);
    proxy->setDSOLocal(false);
    proxy->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    proxy->setAlignment(llvm::Align(alignment));

    CandidateStorage result{
        proxy,
        identity,
        layout,
        size,
        alignment,
        original_name,
    };
    setStorageMetadata(*proxy, result);
    storage.replaceAllUsesWith(proxy);
    storage.eraseFromParent();
    return result;
}

CandidateFunction transformFunction(
    llvm::Module &module,
    llvm::Function &function)
{
    llvm::LLVMContext &context = module.getContext();
    const std::string original_name = function.getName().str();
    const Hash128 identity = cvite::transform::hash128(
        cvite::transform::identitySeed(module, function, original_name));
    const Hash128 abi = cvite::transform::hash128(
        cvite::transform::abiSeed(module, function));
    const Hash128 implementation_fingerprint = cvite::transform::hash128(
        cvite::transform::implementationSeed(function));
    const bool entry_address_escapes = hasAddressEscape(function);
    const llvm::GlobalValue::LinkageTypes original_linkage = function.getLinkage();
    const llvm::GlobalValue::VisibilityTypes original_visibility =
        function.getVisibility();
    const llvm::GlobalValue::DLLStorageClassTypes original_dll_storage =
        function.getDLLStorageClass();
    const bool original_dso_local = function.isDSOLocal();
    llvm::Comdat *original_comdat = function.getComdat();
    const std::string original_section = function.getSection().str();
    const llvm::MaybeAlign original_alignment = function.getAlign();
    const llvm::AttributeList call_attributes = abiAttributes(function);

    function.setName("__cvite_patch." + identity.hex);
    llvm::Function *entry = llvm::Function::Create(
        function.getFunctionType(),
        original_linkage,
        function.getAddressSpace(),
        original_name,
        &module);
    entry->setCallingConv(function.getCallingConv());
    entry->setVisibility(original_visibility);
    entry->setDLLStorageClass(original_dll_storage);
    entry->setDSOLocal(original_dso_local);
    entry->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    entry->setAttributes(call_attributes.addFnAttribute(
        context, llvm::Attribute::NoInline));
    if (!original_section.empty()) {
        entry->setSection(original_section);
    }
    if (original_alignment.has_value()) {
        entry->setAlignment(*original_alignment);
    }
    if (original_comdat != nullptr) {
        entry->setComdat(original_comdat);
    }
    copyArgumentNames(*entry, function);

    /*
     * Recursive calls, calls to another function in the same candidate, and
     * function-address expressions all resolve through a generated entry. The
     * entry asks the baseline host for the target from the active immutable
     * dispatch snapshot, so a candidate never hard-wires another candidate's
     * implementation address into its code.
     */
    function.replaceAllUsesWith(entry);
    function.setLinkage(llvm::GlobalValue::InternalLinkage);
    function.setVisibility(llvm::GlobalValue::DefaultVisibility);
    function.setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    function.setDSOLocal(true);
    function.setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    function.setComdat(nullptr);

    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::BasicBlock *entry_block =
        llvm::BasicBlock::Create(context, "entry", entry);
    llvm::IRBuilder<> builder(entry_block);
    builder.CreateCall(getCallScopeFunction(module, kCallEnter));
    llvm::CallInst *target = builder.CreateCall(
        getTargetFunction(module),
        {
            llvm::ConstantInt::get(i64, identity.high),
            llvm::ConstantInt::get(i64, identity.low),
            llvm::ConstantInt::get(i64, abi.high),
            llvm::ConstantInt::get(i64, abi.low),
        },
        "cvite.target");

    llvm::SmallVector<llvm::Value *, 8> arguments;
    arguments.reserve(entry->arg_size());
    for (llvm::Argument &argument : entry->args()) {
        arguments.push_back(&argument);
    }

    llvm::CallInst *call = builder.CreateCall(
        function.getFunctionType(), target, arguments);
    call->setCallingConv(function.getCallingConv());
    call->setAttributes(call_attributes);
    builder.CreateCall(getCallScopeFunction(module, kCallLeave));
    if (function.getReturnType()->isVoidTy()) {
        builder.CreateRetVoid();
    } else {
        builder.CreateRet(call);
    }

    setRefreshMetadata(function, identity, abi);
    setRefreshMetadata(*entry, identity, abi);
    cvite::transform::setImplementationMetadata(
        function, implementation_fingerprint);
    return {
        &function,
        entry,
        identity,
        abi,
        implementation_fingerprint,
        entry_address_escapes,
        original_name,
    };
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
    llvm::ArrayRef<CandidateFunction> functions)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *record_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, i64, i64, pointer, pointer},
        false);

    llvm::SmallVector<llvm::Constant *, 32> records;
    records.reserve(functions.size());
    for (const CandidateFunction &function : functions) {
        llvm::GlobalVariable *debug_name = createDebugName(
            module,
            "__cvite_candidate_name.",
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
    llvm::ArrayRef<CandidateStorage> storages)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *record_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, i64, i64, pointer},
        false);

    llvm::SmallVector<llvm::Constant *, 32> records;
    records.reserve(storages.size());
    for (const CandidateStorage &storage : storages) {
        llvm::GlobalVariable *debug_name = createDebugName(
            module,
            "__cvite_candidate_storage_name.",
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
    llvm::ArrayRef<CandidateFunction> functions,
    llvm::ArrayRef<CandidateStorage> storages)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::GlobalVariable *function_records =
        createFunctionRecords(module, functions);
    llvm::GlobalVariable *storage_records =
        createStorageRecords(module, storages);

    std::uint64_t flags = 0U;
    for (const CandidateFunction &function : functions) {
        if (function.entry_address_escapes) {
            flags |= CVITE_CANDIDATE_FLAG_ENTRY_ADDRESS_ESCAPES;
        }
    }

    llvm::StructType *manifest_type = llvm::StructType::get(
        context, {i64, i64, i64, pointer, i64, pointer}, false);
    llvm::Constant *manifest_initializer = llvm::ConstantStruct::get(
        manifest_type,
        {
            llvm::ConstantInt::get(i64, kCandidateSchema),
            llvm::ConstantInt::get(i64, flags),
            llvm::ConstantInt::get(
                i64, static_cast<std::uint64_t>(functions.size())),
            function_records,
            llvm::ConstantInt::get(
                i64, static_cast<std::uint64_t>(storages.size())),
            storage_records,
        });
    auto *manifest = new llvm::GlobalVariable(
        module,
        manifest_type,
        true,
        llvm::GlobalValue::ExternalLinkage,
        manifest_initializer,
        kManifestSymbol);
    manifest->setVisibility(llvm::GlobalValue::DefaultVisibility);
    manifest->setDSOLocal(true);
    manifest->setAlignment(llvm::Align(8U));

    llvm::SmallVector<llvm::GlobalValue *, 1> retained;
    retained.push_back(manifest);
    llvm::appendToCompilerUsed(module, retained);
}

class CViteCandidatePass final
    : public llvm::PassInfoMixin<CViteCandidatePass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        if (module.getModuleFlag(kCandidateFlag) != nullptr) {
            return llvm::PreservedAnalyses::all();
        }
        if (module.getModuleFlag(kBaselineFlag) != nullptr) {
            module.getContext().emitError(
                "CVite baseline and candidate transforms are mutually exclusive");
            return llvm::PreservedAnalyses::all();
        }

        llvm::SmallVector<llvm::GlobalVariable *, 32> storage_candidates;
        for (llvm::GlobalVariable &global : module.globals()) {
            if (cvite::transform::shouldTrackStorageDefinition(global)) {
                storage_candidates.push_back(&global);
            }
        }
        llvm::SmallVector<CandidateStorage, 32> storages;
        storages.reserve(storage_candidates.size());
        for (llvm::GlobalVariable *storage : storage_candidates) {
            storages.push_back(transformStorage(module, *storage));
        }

        llvm::SmallVector<llvm::Function *, 32> candidates;
        for (llvm::Function &function : module) {
            if (shouldTransform(function)) {
                candidates.push_back(&function);
            }
        }

        llvm::SmallVector<CandidateFunction, 32> transformed;
        transformed.reserve(candidates.size());
        for (llvm::Function *function : candidates) {
            transformed.push_back(transformFunction(module, *function));
        }

        createManifest(module, transformed, storages);
        module.addModuleFlag(
            llvm::Module::Error,
            kCandidateFlag,
            kCandidateSchema);
        return llvm::PreservedAnalyses::none();
    }

    static bool isRequired() { return true; }
};

} // namespace

void cviteRegisterCandidatePass(llvm::PassBuilder &builder)
{
    builder.registerPipelineParsingCallback(
        [](llvm::StringRef name,
           llvm::ModulePassManager &manager,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != kPassName) {
                return false;
            }
            manager.addPass(CViteCandidatePass());
            return true;
        });
}
