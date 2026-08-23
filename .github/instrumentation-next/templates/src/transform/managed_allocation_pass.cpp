#include "managed_allocation_pass.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

#include <cstdint>

namespace {

constexpr llvm::StringLiteral kPassName = "cvite-managed-allocation";
constexpr llvm::StringLiteral kAllocationMetadata = "cvite.managed.alloc";
constexpr llvm::StringLiteral kSchemaFlag = "cvite.managed-allocation.schema";
constexpr unsigned kSchema = 1U;

constexpr llvm::StringLiteral kAllocate =
    "__cvite_host_managed_allocate";
constexpr llvm::StringLiteral kCallocate =
    "__cvite_host_managed_callocate";
constexpr llvm::StringLiteral kFree = "__cvite_host_managed_free";
constexpr llvm::StringLiteral kTrackPointer =
    "__cvite_host_managed_track_pointer";
constexpr llvm::StringLiteral kEscapePointer =
    "__cvite_host_managed_escape_pointer";

struct AllocationMetadata final {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;
    std::uint64_t alignment = 0U;
};

llvm::FunctionCallee getAllocate(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    return module.getOrInsertFunction(
        kAllocate,
        llvm::FunctionType::get(
            pointer, {i64, i64, i64, i64}, false));
}

llvm::FunctionCallee getCallocate(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    return module.getOrInsertFunction(
        kCallocate,
        llvm::FunctionType::get(
            pointer, {i64, i64, i64, i64, i64}, false));
}

llvm::FunctionCallee getFree(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    return module.getOrInsertFunction(
        kFree,
        llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {pointer}, false));
}

llvm::FunctionCallee getTrackPointer(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    return module.getOrInsertFunction(
        kTrackPointer,
        llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {pointer, pointer}, false));
}

llvm::FunctionCallee getEscapePointer(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    return module.getOrInsertFunction(
        kEscapePointer,
        llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {pointer}, false));
}

bool metadataInteger(
    llvm::MDNode &metadata,
    unsigned index,
    std::uint64_t &value)
{
    if (index >= metadata.getNumOperands()) {
        return false;
    }
    auto *constant = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
        metadata.getOperand(index));
    if (constant == nullptr) {
        return false;
    }
    value = constant->getZExtValue();
    return true;
}

bool readMetadata(
    llvm::CallBase &call,
    AllocationMetadata &metadata)
{
    llvm::MDNode *node = call.getMetadata(kAllocationMetadata);
    return node != nullptr && metadataInteger(*node, 0U, metadata.high) &&
        metadataInteger(*node, 1U, metadata.low) &&
        metadataInteger(*node, 2U, metadata.alignment) &&
        (metadata.high != 0U || metadata.low != 0U);
}

bool generatedName(llvm::StringRef name)
{
    return name.starts_with("__cvite_");
}

bool recognizedAllocator(llvm::StringRef name)
{
    return name == "malloc" || name == "calloc" || name == "realloc" ||
        name == "aligned_alloc";
}

bool shouldEscapeAcrossCall(const llvm::CallBase &call)
{
    const llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
        return true;
    }
    if (callee->isIntrinsic() || generatedName(callee->getName()) ||
        recognizedAllocator(callee->getName()) || callee->getName() == "free") {
        return false;
    }
    return callee->isDeclaration();
}

void copyCallProperties(
    llvm::CallBase &source,
    llvm::CallInst &destination)
{
    destination.setDebugLoc(source.getDebugLoc());
    destination.setCallingConv(source.getCallingConv());
    if (source.hasName()) {
        destination.setName(source.getName());
    }
}

bool rewriteAllocations(llvm::Module &module)
{
    llvm::SmallVector<llvm::CallBase *, 32> calls;
    for (llvm::Function &function : module) {
        if (function.isDeclaration() || generatedName(function.getName())) {
            continue;
        }
        for (llvm::BasicBlock &block : function) {
            for (llvm::Instruction &instruction : block) {
                if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    calls.push_back(call);
                }
            }
        }
    }

    bool changed = false;
    llvm::Type *i64 = llvm::Type::getInt64Ty(module.getContext());
    for (llvm::CallBase *call : calls) {
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr) {
            continue;
        }
        const llvm::StringRef name = callee->getName();
        if (name == "free" && call->arg_size() == 1U) {
            llvm::IRBuilder<> builder(call);
            llvm::CallInst *replacement = builder.CreateCall(
                getFree(module), {call->getArgOperand(0U)});
            copyCallProperties(*call, *replacement);
            call->eraseFromParent();
            changed = true;
            continue;
        }

        AllocationMetadata metadata;
        if (!readMetadata(*call, metadata)) {
            continue;
        }
        llvm::IRBuilder<> builder(call);
        llvm::CallInst *replacement = nullptr;
        if (name == "malloc" && call->arg_size() == 1U) {
            replacement = builder.CreateCall(
                getAllocate(module),
                {
                    llvm::ConstantInt::get(i64, metadata.high),
                    llvm::ConstantInt::get(i64, metadata.low),
                    call->getArgOperand(0U),
                    llvm::ConstantInt::get(i64, metadata.alignment),
                });
        } else if (name == "calloc" && call->arg_size() == 2U) {
            replacement = builder.CreateCall(
                getCallocate(module),
                {
                    llvm::ConstantInt::get(i64, metadata.high),
                    llvm::ConstantInt::get(i64, metadata.low),
                    call->getArgOperand(0U),
                    call->getArgOperand(1U),
                    llvm::ConstantInt::get(i64, metadata.alignment),
                });
        }
        if (replacement == nullptr) {
            continue;
        }
        copyCallProperties(*call, *replacement);
        call->replaceAllUsesWith(replacement);
        call->eraseFromParent();
        changed = true;
    }
    return changed;
}

bool instrumentPointerProvenance(llvm::Module &module)
{
    llvm::SmallVector<llvm::StoreInst *, 32> stores;
    llvm::SmallVector<llvm::CallBase *, 32> calls;
    llvm::SmallVector<llvm::ReturnInst *, 16> returns;
    llvm::SmallVector<llvm::PtrToIntInst *, 16> conversions;

    for (llvm::Function &function : module) {
        if (function.isDeclaration() || generatedName(function.getName())) {
            continue;
        }
        for (llvm::BasicBlock &block : function) {
            for (llvm::Instruction &instruction : block) {
                if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    if (store->getValueOperand()->getType()->isPointerTy()) {
                        stores.push_back(store);
                    }
                } else if (auto *call =
                               llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    calls.push_back(call);
                } else if (auto *ret =
                               llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
                    if (ret->getReturnValue() != nullptr &&
                        ret->getReturnValue()->getType()->isPointerTy()) {
                        returns.push_back(ret);
                    }
                } else if (auto *conversion =
                               llvm::dyn_cast<llvm::PtrToIntInst>(&instruction)) {
                    conversions.push_back(conversion);
                }
            }
        }
    }

    bool changed = false;
    for (llvm::StoreInst *store : stores) {
        llvm::Value *stored = store->getValueOperand();
        llvm::Value *destination = llvm::getUnderlyingObject(
            store->getPointerOperand());
        if (llvm::isa<llvm::GlobalVariable>(destination)) {
            llvm::IRBuilder<> builder(store->getNextNode());
            llvm::CallInst *track = builder.CreateCall(
                getTrackPointer(module),
                {store->getPointerOperand(), stored});
            track->setDebugLoc(store->getDebugLoc());
            changed = true;
            continue;
        }
        if (llvm::isa<llvm::AllocaInst>(destination) &&
            store->getFunction()->getName() != "main") {
            continue;
        }
        llvm::IRBuilder<> builder(store);
        llvm::CallInst *escape =
            builder.CreateCall(getEscapePointer(module), {stored});
        escape->setDebugLoc(store->getDebugLoc());
        changed = true;
    }

    for (llvm::CallBase *call : calls) {
        llvm::Function *callee = call->getCalledFunction();
        if (callee != nullptr && generatedName(callee->getName())) {
            continue;
        }
        if (!shouldEscapeAcrossCall(*call)) {
            continue;
        }
        llvm::IRBuilder<> builder(call);
        for (llvm::Use &argument : call->args()) {
            llvm::Value *value = argument.get();
            if (!value->getType()->isPointerTy()) {
                continue;
            }
            llvm::CallInst *escape =
                builder.CreateCall(getEscapePointer(module), {value});
            escape->setDebugLoc(call->getDebugLoc());
            changed = true;
        }
    }

    for (llvm::ReturnInst *ret : returns) {
        llvm::IRBuilder<> builder(ret);
        llvm::CallInst *escape = builder.CreateCall(
            getEscapePointer(module), {ret->getReturnValue()});
        escape->setDebugLoc(ret->getDebugLoc());
        changed = true;
    }
    for (llvm::PtrToIntInst *conversion : conversions) {
        llvm::IRBuilder<> builder(conversion);
        llvm::CallInst *escape = builder.CreateCall(
            getEscapePointer(module), {conversion->getPointerOperand()});
        escape->setDebugLoc(conversion->getDebugLoc());
        changed = true;
    }
    return changed;
}

class CViteManagedAllocationPass final
    : public llvm::PassInfoMixin<CViteManagedAllocationPass> {
public:
    llvm::PreservedAnalyses run(
        llvm::Module &module,
        llvm::ModuleAnalysisManager &)
    {
        if (module.getModuleFlag(kSchemaFlag) != nullptr) {
            return llvm::PreservedAnalyses::all();
        }
        bool changed = rewriteAllocations(module);
        changed = instrumentPointerProvenance(module) || changed;
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

void cviteRegisterManagedAllocationPass(llvm::PassBuilder &builder)
{
    builder.registerPipelineParsingCallback(
        [](llvm::StringRef name,
           llvm::ModulePassManager &manager,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != kPassName) {
                return false;
            }
            manager.addPass(CViteManagedAllocationPass());
            return true;
        });
}
