#ifndef CVITE_BASELINE_MANIFEST_PASS_H
#define CVITE_BASELINE_MANIFEST_PASS_H

namespace llvm {
class PassBuilder;
}

void cviteRegisterBaselineManifestPass(llvm::PassBuilder &builder);

#endif
