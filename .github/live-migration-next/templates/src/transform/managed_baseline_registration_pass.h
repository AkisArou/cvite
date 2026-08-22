#ifndef CVITE_MANAGED_BASELINE_REGISTRATION_PASS_H
#define CVITE_MANAGED_BASELINE_REGISTRATION_PASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class PassBuilder;
}

void cviteRegisterManagedBaselineRegistrationPass(
    llvm::PassBuilder &builder);
void cviteAppendManagedBaselineRegistrationPass(
    llvm::ModulePassManager &manager);

#endif
