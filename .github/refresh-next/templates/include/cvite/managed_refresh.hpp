#ifndef CVITE_MANAGED_REFRESH_HPP
#define CVITE_MANAGED_REFRESH_HPP

#include "cvite/id.h"
#include "cvite/managed_memory.h"
#include "cvite/semantic_index.hpp"

#include <cstddef>
#include <string>

namespace cvite::refresh {

struct ManagedRefreshResult final {
    bool applied = false;
    bool restart_required = false;
    std::size_t migrated_objects = 0U;
    cvite_managed_status managed_status = CVITE_MANAGED_OK;
    std::string diagnostic;
};

/*
 * The caller must hold the process-wide CVite quiescence/writer gate. This
 * function proves only record-layout compatibility and delegates transactional
 * object/pointer publication to cvite_managed_domain.
 */
ManagedRefreshResult migrateRecord(
    cvite_managed_domain *domain,
    cvite_id managed_type,
    const cvite::semantic::SemanticIndex &old_index,
    const cvite::semantic::SemanticIndex &new_index,
    const std::string &record_id_or_name);

} // namespace cvite::refresh

#endif
