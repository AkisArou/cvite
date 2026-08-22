#ifndef CVITE_MANAGED_TYPE_EMITTER_HPP
#define CVITE_MANAGED_TYPE_EMITTER_HPP

#include "cvite/allocation_index.hpp"
#include "cvite/semantic_index.hpp"

#include <string>

namespace llvm {
class Module;
}

namespace cvite::compiler {

bool emitManagedTypeManifest(
    llvm::Module &module,
    const cvite::semantic::AllocationIndex &allocations,
    const cvite::semantic::SemanticIndex &semantics,
    std::string &error);

} // namespace cvite::compiler

#endif
