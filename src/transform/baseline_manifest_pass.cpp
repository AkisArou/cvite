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
constexpr llvm::StringLiteral kRecordsSymbol = "__cvite_baseline_records";
constexpr llvm::StringLiteral kProgramMainSymbol = "__cvite_program_main";
constexpr unsigned kManifestSchema = 1U;

struct BaselineFunction final {
    llvm::Function *implementation = nullptr;
    llvm::GlobalVariable *slot = nullptr;
    Hash128 identity;
    Hash128 abi;
    std::string debug_name;
};

bool parseHash(llvm::StringRef hex, Hash128 &hash)
{
    if (hex.size() != 32U) {
        return false;
    }

    std::uint64_t high = 0U;
    std::uint64_t low = 0U;
    if (hex.take_front(16U).getAsInteger(16U, high) ||
        hex.drop_front(16U).getAsInteger(16U, low)) {
        return false;
    }

    hash = Hash128{high, low, hex.str()};
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

    result = BaselineFunction{
        implementation,
        slot,
        identity,
        Hash128{
            abi_high->getZExtValue(),
            abi_low->getZExtValue(),
            std::string(),
        },
        entry.getName().str(),
    };
    return true;
}

llvm::GlobalVariable *createDebugName(
    llvm::Module &module,
    const BaselineFunction &function)
{
    llvm::Constant *data = llvm::ConstantDataArray::getString(
        module.getContext(), function.debug_name, true);
    auto *global = new llvm::GlobalVariable(
        module,
        data->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        data,
        "__cvite_baseline_name." + function.identity.hex);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(1U));
    return global;
}

void createManifest(
    llvm::Module &module,
    llvm::ArrayRef<BaselineFunction> functions)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *record_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, pointer, pointer, pointer},
        false);

    llvm::SmallVector<llvm::Constant *, 32> records;
    records.reserve(functions.size());
    for (const BaselineFunction &function : functions) {
        llvm::GlobalVariable *debug_name = createDebugName(module, function);
        records.push_back(llvm::ConstantStruct::get(
            record_type,
            {
                llvm::ConstantInt::get(i64, function.identity.high),
                llvm::ConstantInt::get(i64, function.identity.low),
                llvm::ConstantInt::get(i64, function.abi.high),
                llvm::ConstantInt::get(i64, function.abi.low),
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
        kRecordsSymbol);
    records_global->setAlignment(llvm::Align(8U));

    llvm::StructType *manifest_type = llvm::StructType::get(
        context, {i64, i64, pointer}, false);
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
                records_global,
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

        createManifest(module, functions);
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
