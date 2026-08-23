#ifndef CVITE_CANDIDATE_PASS_H
#define CVITE_CANDIDATE_PASS_H

namespace llvm {
class PassBuilder;
}

void cviteRegisterCandidatePass(llvm::PassBuilder &builder);

#endif
