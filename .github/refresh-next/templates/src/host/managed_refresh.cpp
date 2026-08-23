#include "cvite/managed_refresh.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace cvite::refresh {

ManagedRefreshResult migrateRecord(
    cvite_managed_domain *domain,
    cvite_id managed_type,
    const cvite::semantic::SemanticIndex &old_index,
    const cvite::semantic::SemanticIndex &new_index,
    const std::string &record_id_or_name)
{
    ManagedRefreshResult result;
    if (domain == nullptr) {
        result.restart_required = true;
        result.managed_status = CVITE_MANAGED_INVALID_ARGUMENT;
        result.diagnostic = "managed refresh domain is null";
        return result;
    }

    const cvite::semantic::RecordInfo *old_record =
        cvite::semantic::findRecord(old_index, record_id_or_name);
    const cvite::semantic::RecordInfo *new_record =
        cvite::semantic::findRecord(new_index, record_id_or_name);
    if (old_record == nullptr || new_record == nullptr) {
        result.restart_required = true;
        result.managed_status = CVITE_MANAGED_NOT_FOUND;
        result.diagnostic =
            "record is not present in both semantic generations: " +
            record_id_or_name;
        return result;
    }

    const cvite::semantic::MigrationPlan semantic_plan =
        cvite::semantic::planManagedAppendOnly(*old_record, *new_record);
    if (!semantic_plan.safe) {
        result.restart_required = true;
        result.managed_status = CVITE_MANAGED_INVALID_STATE;
        result.diagnostic = cvite::semantic::formatPlan(semantic_plan);
        return result;
    }

    std::vector<cvite_managed_copy_operation> copies;
    copies.reserve(semantic_plan.copies.size());
    for (const cvite::semantic::CopyOperation &copy : semantic_plan.copies) {
        copies.push_back({copy.old_offset, copy.new_offset, copy.size});
    }
    const cvite_managed_byte_plan byte_plan = {
        semantic_plan.old_size,
        semantic_plan.new_size,
        semantic_plan.alignment,
        copies.data(),
        copies.size(),
    };

    cvite_managed_error error;
    cvite_managed_error_clear(&error);
    std::size_t migrated = 0U;
    const cvite_managed_status status = cvite_managed_migrate_type(
        domain,
        managed_type,
        semantic_plan.new_size,
        semantic_plan.alignment,
        cvite_managed_apply_byte_plan,
        const_cast<cvite_managed_byte_plan *>(&byte_plan),
        &migrated,
        &error);
    result.managed_status = status;
    result.migrated_objects = migrated;
    if (status != CVITE_MANAGED_OK) {
        result.restart_required = true;
        std::ostringstream diagnostic;
        diagnostic << cvite::semantic::formatPlan(semantic_plan)
                   << "  managed publication rejected: "
                   << cvite_managed_status_name(status);
        if (error.message[0] != '\0') {
            diagnostic << ": " << error.message;
        }
        diagnostic << '\n';
        result.diagnostic = diagnostic.str();
        return result;
    }

    result.applied = true;
    std::ostringstream diagnostic;
    diagnostic << cvite::semantic::formatPlan(semantic_plan)
               << "  migrated objects: " << migrated << '\n';
    result.diagnostic = diagnostic.str();
    return result;
}

} // namespace cvite::refresh
