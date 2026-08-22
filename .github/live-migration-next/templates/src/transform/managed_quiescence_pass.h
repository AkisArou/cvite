#ifndef CVITE_MANAGED_QUIESCENCE_PASS_H
#define CVITE_MANAGED_QUIESCENCE_PASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class PassBuilder;
}

void cviteRegisterManagedQuiescencePass(llvm::PassBuilder &builder);
void cviteAppendManagedQuiescencePass(llvm::ModulePassManager &manager);

#endif
