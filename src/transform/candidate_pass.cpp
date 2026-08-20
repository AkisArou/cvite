#include "candidate_pass.h"
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
constexpr llvm::StringLiteral kManifestSymbol = "__cvite_candidate_manifest";
constexpr llvm::StringLiteral kRecordsSymbol = "__cvite_candidate_records";
constexpr unsigned kCandidateSchema = 1U;

struct CandidateFunction final {
    llvm::Function *implementation = nullptr;
    llvm::Function *entry = nullptr;
    Hash128 identity;
    Hash128 abi;
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
    call->setTailCallKind(llvm::CallInst::TCK_Tail);
    if (function.getReturnType()->isVoidTy()) {
        builder.CreateRetVoid();
    } else {
        builder.CreateRet(call);
    }

    setRefreshMetadata(function, identity, abi);
    setRefreshMetadata(*entry, identity, abi);
    return {&function, entry, identity, abi, original_name};
}

llvm::GlobalVariable *createDebugName(
    llvm::Module &module,
    const CandidateFunction &function)
{
    llvm::Constant *data = llvm::ConstantDataArray::getString(
        module.getContext(), function.debug_name, true);
    auto *global = new llvm::GlobalVariable(
        module,
        data->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        data,
        "__cvite_candidate_name." + function.identity.hex);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(1U));
    return global;
}

void createManifest(
    llvm::Module &module,
    llvm::ArrayRef<CandidateFunction> functions)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *record_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, pointer, pointer},
        false);

    llvm::SmallVector<llvm::Constant *, 32> records;
    records.reserve(functions.size());
    for (const CandidateFunction &function : functions) {
        llvm::GlobalVariable *debug_name = createDebugName(module, function);
        records.push_back(llvm::ConstantStruct::get(
            record_type,
            {
                llvm::ConstantInt::get(i64, function.identity.high),
                llvm::ConstantInt::get(i64, function.identity.low),
                llvm::ConstantInt::get(i64, function.abi.high),
                llvm::ConstantInt::get(i64, function.abi.low),
                function.implementation,
                debug_name,
            }));
    }

    llvm::ArrayType *records_type = llvm::ArrayType::get(
        record_type, static_cast<std::uint64_t>(records.size()));
    llvm::Constant *records_initializer =
        llvm::ConstantArray::get(records_type, records);
    auto *records_global = new llvm::GlobalVariable(
        module,
        records_type,
        true,
        llvm::GlobalValue::InternalLinkage,
        records_initializer,
        kRecordsSymbol);
    records_global->setAlignment(llvm::Align(8U));

    llvm::StructType *manifest_type = llvm::StructType::get(
        context, {i64, i64, pointer}, false);
    llvm::Constant *manifest_initializer = llvm::ConstantStruct::get(
        manifest_type,
        {
            llvm::ConstantInt::get(i64, kCandidateSchema),
            llvm::ConstantInt::get(
                i64, static_cast<std::uint64_t>(functions.size())),
            records_global,
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

        createManifest(module, transformed);
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
