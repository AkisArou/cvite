#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Config/llvm-config.h"

#include <cstdint>
#include <string>

namespace {

constexpr llvm::StringLiteral kPassName = "cvite-lowering";
constexpr llvm::StringLiteral kAbiSchema = "cvite.lowered-abi.v1";
constexpr llvm::StringLiteral kFunctionMetadata = "cvite.abi";
constexpr llvm::StringLiteral kFunctionIndex = "cvite.functions";

std::string printType(const llvm::Type &type)
{
    std::string text;
    llvm::raw_string_ostream output(text);
    type.print(output);
    return output.str();
}

void appendAttributeSet(
    llvm::raw_ostream &output,
    llvm::StringRef label,
    llvm::AttributeSet attributes)
{
    output << label << '=';
    output << attributes.getAsString();
    output << '\n';
}

std::string abiSeed(const llvm::Module &module, const llvm::Function &function)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);
    const llvm::AttributeList attributes = function.getAttributes();

    output << kAbiSchema << '\n';
    output << "target=" << module.getTargetTriple() << '\n';
    output << "data-layout=" << module.getDataLayoutStr() << '\n';
    output << "function-type=" << printType(*function.getFunctionType()) << '\n';
    output << "calling-convention=" << function.getCallingConv() << '\n';
    appendAttributeSet(output, "return-attributes", attributes.getRetAttrs());
    for (unsigned index = 0U; index < function.arg_size(); ++index) {
        output << "parameter=" << index << '\n';
        appendAttributeSet(
            output,
            "parameter-attributes",
            attributes.getParamAttrs(index));
    }
    return output.str();
}

std::string md5(llvm::StringRef input)
{
    llvm::MD5 hash;
    llvm::MD5::MD5Result result;
    llvm::SmallString<32> output;

    hash.update(input);
    hash.final(result);
    llvm::MD5::stringifyResult(result, output);
    return output.str().str();
}

bool shouldIndex(const llvm::Function &function)
{
    return !function.isDeclaration() && !function.isIntrinsic() &&
        !function.getName().starts_with("__cvite_");
}

class CViteLoweringPass final
    : public llvm::PassInfoMixin<CViteLoweringPass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        llvm::LLVMContext &context = module.getContext();
        llvm::NamedMDNode *index = module.getNamedMetadata(kFunctionIndex);
        if (index != nullptr) {
            index->eraseFromParent();
        }
        index = module.getOrInsertNamedMetadata(kFunctionIndex);

        bool changed = false;
        for (llvm::Function &function : module) {
            if (!shouldIndex(function)) {
                continue;
            }

            const std::string seed = abiSeed(module, function);
            const std::string fingerprint = md5(seed);
            llvm::Metadata *function_metadata[] = {
                llvm::MDString::get(context, fingerprint),
                llvm::MDString::get(context, kAbiSchema),
            };
            function.setMetadata(
                kFunctionMetadata,
                llvm::MDNode::get(context, function_metadata));

            llvm::Metadata *index_metadata[] = {
                llvm::MDString::get(context, function.getName()),
                llvm::MDString::get(context, fingerprint),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(context),
                    static_cast<std::uint64_t>(function.getCallingConv()))),
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
