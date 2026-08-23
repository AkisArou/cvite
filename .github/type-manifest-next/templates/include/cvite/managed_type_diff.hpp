#ifndef CVITE_MANAGED_TYPE_DIFF_HPP
#define CVITE_MANAGED_TYPE_DIFF_HPP

#include "cvite/managed_memory.h"
#include "cvite/managed_type.h"

#include <string>
#include <vector>

namespace cvite::refresh {

enum class ManagedTypeCompatibility {
    identical,
    append_only,
    incompatible,
};

struct ManagedTypePlan final {
    ManagedTypeCompatibility compatibility =
        ManagedTypeCompatibility::incompatible;
    cvite_managed_byte_plan byte_plan{};
    std::vector<cvite_managed_copy_operation> copies;
    std::string diagnostic;
};

ManagedTypePlan planManagedTypeEvolution(
    const cvite_managed_type_record &old_type,
    const cvite_managed_type_record &new_type);

const cvite_managed_type_record *findManagedType(
    const cvite_managed_type_manifest &manifest,
    std::uint64_t id_high,
    std::uint64_t id_low);

} // namespace cvite::refresh

#endif
