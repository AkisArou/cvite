#include "baseline_manifest_pass.h"
#include "candidate_pass.h"
#include "stable_entry_pass.h"
#include "transform_support.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Config/llvm-config.h"

#include <cstdint>
#include <string>

namespace {

constexpr llvm::StringLiteral kPassName = "cvite-lowering";

class CViteLoweringPass final
    : public llvm::PassInfoMixin<CViteLoweringPass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        llvm::LLVMContext &context = module.getContext();
        llvm::NamedMDNode *index =
            module.getNamedMetadata(cvite::transform::kFunctionIndex);
        if (index != nullptr) {
            index->eraseFromParent();
        }
        index = module.getOrInsertNamedMetadata(
            cvite::transform::kFunctionIndex);

        bool changed = false;
        for (llvm::Function &function : module) {
            if (!cvite::transform::shouldIndex(function)) {
                continue;
            }

            const cvite::transform::Hash128 fingerprint =
                cvite::transform::hash128(
                    cvite::transform::abiSeed(module, function));
            llvm::Metadata *function_metadata[] = {
                llvm::MDString::get(context, fingerprint.hex),
                llvm::MDString::get(context, cvite::transform::kAbiSchema),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(context), fingerprint.high)),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(context), fingerprint.low)),
            };
            function.setMetadata(
                cvite::transform::kFunctionMetadata,
                llvm::MDNode::get(context, function_metadata));

            llvm::Metadata *index_metadata[] = {
                llvm::MDString::get(context, function.getName()),
                llvm::MDString::get(context, fingerprint.hex),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(context),
                    static_cast<std::uint64_t>(function.getCallingConv()))),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(context), fingerprint.high)),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(context), fingerprint.low)),
            };
            index->addOperand(llvm::MDNode::get(context, index_metadata));

            if (function.hasFnAttribute(llvm::Attribute::AlwaysInline)) {
                function.removeFnAttr(llvm::Attribute::AlwaysInline);
            }
            if (!function.hasFnAttribute(llvm::Attribute::NoInline)) {
                function.addFnAttr(llvm::Attribute::NoInline);
            }
            changed = true;
        }

        if (module.getModuleFlag("cvite.lowered-abi.schema") == nullptr) {
            module.addModuleFlag(
                llvm::Module::Error,
                "cvite.lowered-abi.schema",
                1U);
            changed = true;
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

    static bool isRequired() { return true; }
};

void registerCallbacks(llvm::PassBuilder &builder)
{
    builder.registerPipelineParsingCallback(
        [](llvm::StringRef name,
           llvm::ModulePassManager &manager,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != kPassName) {
                return false;
            }
            manager.addPass(CViteLoweringPass());
            return true;
        });

    builder.registerPipelineStartEPCallback(
        [](llvm::ModulePassManager &manager, llvm::OptimizationLevel) {
            manager.addPass(CViteLoweringPass());
        });

    cviteRegisterStableEntryPass(builder);
    cviteRegisterBaselineManifestPass(builder);
    cviteRegisterCandidatePass(builder);
}

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo()
{
    return {
        LLVM_PLUGIN_API_VERSION,
        "CViteLowering",
        LLVM_VERSION_STRING,
        registerCallbacks,
    };
}
