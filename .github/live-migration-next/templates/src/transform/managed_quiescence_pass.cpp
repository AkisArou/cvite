#include "managed_quiescence_pass.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

namespace {

constexpr llvm::StringLiteral kPassName = "cvite-managed-quiescence";
constexpr llvm::StringLiteral kSchemaFlag = "cvite.managed-quiescence.schema";
constexpr unsigned kSchema = 1U;
constexpr llvm::StringLiteral kEnter =
    "__cvite_host_managed_reader_enter";
constexpr llvm::StringLiteral kLeave =
    "__cvite_host_managed_reader_leave";

bool generatedFunction(const llvm::Function &function)
{
    return function.getName().starts_with("__cvite_");
}

llvm::FunctionCallee getHook(
    llvm::Module &module,
    llvm::StringRef name)
{
    return module.getOrInsertFunction(
        name,
        llvm::FunctionType::get(
            llvm::Type::getVoidTy(module.getContext()), false));
}

bool isNonLocalExit(const llvm::CallBase &call)
{
    const llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
        return call.doesNotReturn();
    }
    const llvm::StringRef name = callee->getName();
    return call.doesNotReturn() || name == "longjmp" || name == "_longjmp" ||
        name == "siglongjmp" || name == "pthread_exit";
}

llvm::Instruction *leaveInsertionPoint(llvm::ReturnInst &ret)
{
    llvm::Instruction *previous = ret.getPrevNode();
    auto *call = llvm::dyn_cast_or_null<llvm::CallInst>(previous);
    if (call != nullptr && call->isMustTailCall()) {
        return call;
    }
    return &ret;
}

class CViteManagedQuiescencePass final
    : public llvm::PassInfoMixin<CViteManagedQuiescencePass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        if (module.getModuleFlag(kSchemaFlag) != nullptr) {
            return llvm::PreservedAnalyses::all();
        }

        bool changed = false;
        const llvm::FunctionCallee enter = getHook(module, kEnter);
        const llvm::FunctionCallee leave = getHook(module, kLeave);
        for (llvm::Function &function : module) {
            if (function.isDeclaration() || function.getName() == "main" ||
                generatedFunction(function) ||
                function.hasFnAttribute(llvm::Attribute::Naked)) {
                continue;
            }

            llvm::BasicBlock &entry = function.getEntryBlock();
            const auto insertion = entry.getFirstInsertionPt();
            if (insertion == entry.end()) {
                continue;
            }
            llvm::IRBuilder<> entry_builder(&*insertion);
            llvm::CallInst *enter_call = entry_builder.CreateCall(enter);
            enter_call->setDebugLoc(insertion->getDebugLoc());

            llvm::SmallVector<llvm::ReturnInst *, 8> returns;
            llvm::SmallVector<llvm::CallBase *, 8> nonlocal_exits;
            for (llvm::BasicBlock &block : function) {
                for (llvm::Instruction &instruction : block) {
                    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(
                            &instruction)) {
                        returns.push_back(ret);
                    } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(
                                   &instruction)) {
                        if (call != enter_call && isNonLocalExit(*call)) {
                            nonlocal_exits.push_back(call);
                        }
                    }
                }
            }
            for (llvm::ReturnInst *ret : returns) {
                llvm::IRBuilder<> builder(leaveInsertionPoint(*ret));
                llvm::CallInst *leave_call = builder.CreateCall(leave);
                leave_call->setDebugLoc(ret->getDebugLoc());
            }
            for (llvm::CallBase *call : nonlocal_exits) {
                llvm::IRBuilder<> builder(call);
                llvm::CallInst *leave_call = builder.CreateCall(leave);
                leave_call->setDebugLoc(call->getDebugLoc());
            }
            changed = true;
        }

        module.addModuleFlag(
            llvm::Module::Error,
            kSchemaFlag,
            kSchema);
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

    static bool isRequired() { return true; }
};

} // namespace

void cviteRegisterManagedQuiescencePass(llvm::PassBuilder &builder)
{
    builder.registerPipelineParsingCallback(
        [](llvm::StringRef name,
           llvm::ModulePassManager &manager,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != kPassName) {
                return false;
            }
            manager.addPass(CViteManagedQuiescencePass());
            return true;
        });
}

void cviteAppendManagedQuiescencePass(llvm::ModulePassManager &manager)
{
    manager.addPass(CViteManagedQuiescencePass());
}
