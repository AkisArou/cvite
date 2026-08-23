#include "stable_entry_pass.h"
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
#include <limits>
#include <string>
#include <utility>

namespace {

using cvite::transform::Hash128;

constexpr llvm::StringLiteral kPassName = "cvite-baseline";
constexpr llvm::StringLiteral kBaselineFlag = "cvite.baseline.schema";
constexpr llvm::StringLiteral kRegisterFunction =
    "__cvite_host_register_function";
constexpr llvm::StringLiteral kRegisterStorage =
    "__cvite_host_register_storage";
constexpr llvm::StringLiteral kTargetAt = "__cvite_host_target_at";
constexpr llvm::StringLiteral kCallEnter = "__cvite_host_call_enter";
constexpr llvm::StringLiteral kCallLeave = "__cvite_host_call_leave";
constexpr unsigned kBaselineSchema = 3U;
constexpr int kRegistrationPriority = 1;

struct WrappedFunction final {
    llvm::Function *implementation = nullptr;
    llvm::Function *entry = nullptr;
    llvm::GlobalVariable *slot = nullptr;
    Hash128 identity;
    Hash128 abi;
    Hash128 implementation_fingerprint;
    std::string debug_name;
};

struct TrackedStorage final {
    llvm::GlobalVariable *global = nullptr;
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

bool hasSupportedLinkage(const llvm::Function &function)
{
    return function.hasExternalLinkage() || function.hasInternalLinkage() ||
        function.hasPrivateLinkage();
}

bool shouldWrap(const llvm::Function &function)
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

llvm::FunctionCallee getRegisterFunction(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::FunctionType *type = llvm::FunctionType::get(
        i64,
        {i64, i64, i64, i64, pointer, pointer},
        false);
    return module.getOrInsertFunction(kRegisterFunction, type);
}

llvm::FunctionCallee getRegisterStorageFunction(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::FunctionType *type = llvm::FunctionType::get(
        pointer,
        {i64, i64, i64, i64, i64, i64, pointer, pointer},
        false);
    return module.getOrInsertFunction(kRegisterStorage, type);
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
    llvm::FunctionType *type = llvm::FunctionType::get(pointer, {i64}, false);
    return module.getOrInsertFunction(kTargetAt, type);
}

void copyArgumentNames(llvm::Function &destination, const llvm::Function &source)
{
    auto destination_argument = destination.arg_begin();
    for (const llvm::Argument &source_argument : source.args()) {
        destination_argument->setName(source_argument.getName());
        ++destination_argument;
    }
}

void setStorageMetadata(
    llvm::GlobalVariable &global,
    const TrackedStorage &storage)
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

TrackedStorage trackStorage(
    llvm::Module &module,
    llvm::GlobalVariable &global)
{
    const std::string original_name = global.getName().str();
    TrackedStorage storage{
        &global,
        cvite::transform::hash128(cvite::transform::storageIdentitySeed(
            module, global, original_name)),
        cvite::transform::hash128(
            cvite::transform::storageLayoutSeed(module, global)),
        cvite::transform::storageSize(module, global),
        cvite::transform::storageAlignment(module, global),
        original_name,
    };
    setStorageMetadata(global, storage);
    return storage;
}

WrappedFunction wrapFunction(llvm::Module &module, llvm::Function &function)
{
    llvm::LLVMContext &context = module.getContext();
    const std::string original_name = function.getName().str();
    const Hash128 identity = cvite::transform::hash128(
        cvite::transform::identitySeed(module, function, original_name));
    const Hash128 abi = cvite::transform::hash128(
        cvite::transform::abiSeed(module, function));
    const Hash128 implementation_fingerprint = cvite::transform::hash128(
        cvite::transform::implementationSeed(function));
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

    function.setName("__cvite_impl." + identity.hex);
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

    function.replaceAllUsesWith(entry);
    function.setLinkage(llvm::GlobalValue::InternalLinkage);
    function.setVisibility(llvm::GlobalValue::DefaultVisibility);
    function.setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    function.setDSOLocal(true);
    function.setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    function.setComdat(nullptr);

    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    auto *slot = new llvm::GlobalVariable(
        module,
        i64,
        false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i64, std::numeric_limits<std::uint64_t>::max()),
        "__cvite_slot." + identity.hex);
    slot->setAlignment(llvm::Align(8U));

    llvm::BasicBlock *entry_block =
        llvm::BasicBlock::Create(context, "entry", entry);
    llvm::IRBuilder<> builder(entry_block);
    builder.CreateCall(getCallScopeFunction(module, kCallEnter));
    llvm::LoadInst *slot_value = builder.CreateLoad(i64, slot, "cvite.slot");
    llvm::CallInst *target = builder.CreateCall(
        getTargetFunction(module), {slot_value}, "cvite.target");

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

    llvm::Metadata *entry_metadata[] = {
        llvm::MDString::get(context, abi.hex),
        llvm::MDString::get(context, cvite::transform::kAbiSchema),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, abi.high)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, abi.low)),
        llvm::MDString::get(context, identity.hex),
    };
    entry->setMetadata(
        cvite::transform::kFunctionMetadata,
        llvm::MDNode::get(context, entry_metadata));
    cvite::transform::setImplementationMetadata(
        function, implementation_fingerprint);

    return {
        &function,
        entry,
        slot,
        identity,
        abi,
        implementation_fingerprint,
        original_name,
    };
}

void createRegistrationConstructor(
    llvm::Module &module,
    llvm::ArrayRef<WrappedFunction> functions,
    llvm::ArrayRef<TrackedStorage> storages)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::FunctionType *constructor_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), false);
    const Hash128 module_id = cvite::transform::hash128(
        module.getModuleIdentifier());
    llvm::Function *constructor = llvm::Function::Create(
        constructor_type,
        llvm::GlobalValue::InternalLinkage,
        "__cvite_module_init." + module_id.hex,
        &module);
    constructor->addFnAttr(llvm::Attribute::NoInline);

    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", constructor);
    llvm::IRBuilder<> builder(entry);
    const llvm::FunctionCallee register_storage =
        getRegisterStorageFunction(module);
    const llvm::FunctionCallee register_function = getRegisterFunction(module);

    for (const TrackedStorage &storage : storages) {
        llvm::Value *debug_name = builder.CreateGlobalStringPtr(
            storage.debug_name,
            "__cvite_storage_name." + storage.identity.hex);
        builder.CreateCall(
            register_storage,
            {
                llvm::ConstantInt::get(i64, storage.identity.high),
                llvm::ConstantInt::get(i64, storage.identity.low),
                llvm::ConstantInt::get(i64, storage.layout.high),
                llvm::ConstantInt::get(i64, storage.layout.low),
                llvm::ConstantInt::get(i64, storage.size),
                llvm::ConstantInt::get(i64, storage.alignment),
                storage.global,
                debug_name,
            });
    }

    for (const WrappedFunction &function : functions) {
        llvm::Value *debug_name = builder.CreateGlobalStringPtr(
            function.debug_name,
            "__cvite_name." + function.identity.hex);
        llvm::CallInst *slot = builder.CreateCall(
            register_function,
            {
                llvm::ConstantInt::get(i64, function.identity.high),
                llvm::ConstantInt::get(i64, function.identity.low),
                llvm::ConstantInt::get(i64, function.abi.high),
                llvm::ConstantInt::get(i64, function.abi.low),
                function.implementation,
                debug_name,
            },
            "cvite.slot");
        builder.CreateStore(slot, function.slot);
    }
    builder.CreateRetVoid();
    llvm::appendToGlobalCtors(
        module, constructor, kRegistrationPriority, nullptr);
}

class CViteStableEntryPass final
    : public llvm::PassInfoMixin<CViteStableEntryPass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        if (module.getModuleFlag(kBaselineFlag) != nullptr) {
            return llvm::PreservedAnalyses::all();
        }

        llvm::SmallVector<llvm::GlobalVariable *, 32> storage_candidates;
        for (llvm::GlobalVariable &global : module.globals()) {
            if (cvite::transform::shouldTrackStorageDefinition(global)) {
                storage_candidates.push_back(&global);
            }
        }
        llvm::SmallVector<TrackedStorage, 32> storages;
        storages.reserve(storage_candidates.size());
        for (llvm::GlobalVariable *global : storage_candidates) {
            storages.push_back(trackStorage(module, *global));
        }

        llvm::SmallVector<llvm::Function *, 32> candidates;
        for (llvm::Function &function : module) {
            if (shouldWrap(function)) {
                candidates.push_back(&function);
            }
        }

        llvm::SmallVector<WrappedFunction, 32> wrapped;
        wrapped.reserve(candidates.size());
        for (llvm::Function *function : candidates) {
            wrapped.push_back(wrapFunction(module, *function));
        }

        if (!wrapped.empty() || !storages.empty()) {
            createRegistrationConstructor(module, wrapped, storages);
        }
        module.addModuleFlag(
            llvm::Module::Error,
            kBaselineFlag,
            kBaselineSchema);
        return llvm::PreservedAnalyses::none();
    }

    static bool isRequired() { return true; }
};

} // namespace

void cviteRegisterStableEntryPass(llvm::PassBuilder &builder)
{
    builder.registerPipelineParsingCallback(
        [](llvm::StringRef name,
           llvm::ModulePassManager &manager,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != kPassName) {
                return false;
            }
            manager.addPass(CViteStableEntryPass());
            return true;
        });
}
