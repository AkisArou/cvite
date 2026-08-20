#ifndef CVITE_STABLE_ENTRY_PASS_H
#define CVITE_STABLE_ENTRY_PASS_H

namespace llvm {
class PassBuilder;
}

void cviteRegisterStableEntryPass(llvm::PassBuilder &builder);

#endif
