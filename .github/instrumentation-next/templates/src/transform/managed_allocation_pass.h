#ifndef CVITE_MANAGED_ALLOCATION_PASS_H
#define CVITE_MANAGED_ALLOCATION_PASS_H

namespace llvm {
class PassBuilder;
}

void cviteRegisterManagedAllocationPass(llvm::PassBuilder &builder);

#endif
