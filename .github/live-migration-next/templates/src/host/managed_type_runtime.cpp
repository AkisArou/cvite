#include "cvite/managed_memory.h"
#include "cvite/managed_type.h"
#include "cvite/managed_type_diff.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" cvite_managed_domain *__cvite_host_managed_domain_instance(void);
extern "C" int __cvite_host_managed_begin_write(void);
extern "C" void __cvite_host_managed_end_write(void);

namespace {

struct OwnedField final {
    std::uint64_t id_high = 0U;
    std::uint64_t id_low = 0U;
    std::uint64_t type_high = 0U;
    std::uint64_t type_low = 0U;
    std::uint64_t offset = 0U;
    std::uint64_t size = 0U;
    std::uint64_t flags = 0U;
    std::string name;

    cvite_managed_field_record view() const
    {
        return {
            id_high,
            id_low,
            type_high,
            type_low,
            offset,
            size,
            flags,
            name.c_str(),
        };
    }
};

struct OwnedType final {
    std::uint64_t id_high = 0U;
    std::uint64_t id_low = 0U;
    std::uint64_t layout_high = 0U;
    std::uint64_t layout_low = 0U;
    std::uint64_t size = 0U;
    std::uint64_t alignment = 0U;
    std::string name;
    std::vector<OwnedField> fields;

    std::vector<cvite_managed_field_record> fieldViews() const
    {
        std::vector<cvite_managed_field_record> result;
        result.reserve(fields.size());
        for (const OwnedField &field : fields) {
            result.push_back(field.view());
        }
        return result;
    }

    cvite_managed_type_record view(
        const std::vector<cvite_managed_field_record> &views) const
    {
        return {
            id_high,
            id_low,
            layout_high,
            layout_low,
            size,
            alignment,
            static_cast<std::uint64_t>(views.size()),
            views.data(),
            name.c_str(),
        };
    }
};

using TypeKey = std::pair<std::uint64_t, std::uint64_t>;

std::mutex registry_mutex;
std::map<TypeKey, OwnedType> active_types;

bool traceEnabled()
{
    const char *value = std::getenv("CVITE_TRACE_MANAGED");
    return value != nullptr &&
        (std::string(value) == "1" || std::string(value) == "true" ||
         std::string(value) == "yes");
}

void writeMessage(
    char *message,
    std::size_t message_size,
    const std::string &text)
{
    if (message == nullptr || message_size == 0U) {
        return;
    }
    (void)std::snprintf(message, message_size, "%s", text.c_str());
}

bool copyType(
    const cvite_managed_type_record &source,
    OwnedType &destination,
    std::string &error)
{
    if ((source.id_high == 0U && source.id_low == 0U) ||
        source.size == 0U || source.alignment == 0U ||
        (source.field_count > 0U && source.fields == nullptr)) {
        error = "managed type record is invalid";
        return false;
    }
    destination = OwnedType{};
    destination.id_high = source.id_high;
    destination.id_low = source.id_low;
    destination.layout_high = source.layout_high;
    destination.layout_low = source.layout_low;
    destination.size = source.size;
    destination.alignment = source.alignment;
    destination.name = source.debug_name == nullptr
        ? std::string("<anonymous managed type>")
        : std::string(source.debug_name);
    destination.fields.reserve(
        static_cast<std::size_t>(source.field_count));
    for (std::uint64_t index = 0U; index < source.field_count; ++index) {
        const cvite_managed_field_record &field = source.fields[index];
        if ((field.id_high == 0U && field.id_low == 0U) ||
            (field.type_high == 0U && field.type_low == 0U)) {
            error = "managed field record is invalid";
            return false;
        }
        destination.fields.push_back({
            field.id_high,
            field.id_low,
            field.type_high,
            field.type_low,
            field.offset,
            field.size,
            field.flags,
            field.debug_name == nullptr ? std::string()
                                        : std::string(field.debug_name),
        });
    }
    return true;
}

bool validateManifest(
    const cvite_managed_type_manifest *manifest,
    std::string &error)
{
    if (manifest == nullptr) {
        return true;
    }
    if (manifest->schema != CVITE_MANAGED_TYPE_MANIFEST_SCHEMA ||
        (manifest->type_count > 0U && manifest->types == nullptr)) {
        error = "managed type manifest schema or type table is invalid";
        return false;
    }
    return true;
}

struct CandidateType final {
    TypeKey key;
    OwnedType owned;
    cvite::refresh::ManagedTypePlan plan;
    bool existed = false;
};

} // namespace

extern "C" int __cvite_host_register_managed_type_manifest(
    const cvite_managed_type_manifest *manifest,
    char *message,
    std::size_t message_size)
{
    std::string error;
    if (!validateManifest(manifest, error)) {
        writeMessage(message, message_size, error);
        return 0;
    }
    if (manifest == nullptr) {
        return 1;
    }

    std::lock_guard<std::mutex> lock(registry_mutex);
    for (std::uint64_t index = 0U; index < manifest->type_count; ++index) {
        OwnedType candidate;
        if (!copyType(manifest->types[index], candidate, error)) {
            writeMessage(message, message_size, error);
            return 0;
        }
        const TypeKey key{candidate.id_high, candidate.id_low};
        const auto found = active_types.find(key);
        if (found != active_types.end()) {
            const auto old_views = found->second.fieldViews();
            const auto new_views = candidate.fieldViews();
            const auto old_view = found->second.view(old_views);
            const auto new_view = candidate.view(new_views);
            const auto plan = cvite::refresh::planManagedTypeEvolution(
                old_view, new_view);
            if (plan.compatibility !=
                cvite::refresh::ManagedTypeCompatibility::identical) {
                writeMessage(
                    message,
                    message_size,
                    "baseline managed type registration conflicts with active layout: " +
                        plan.diagnostic);
                return 0;
            }
            continue;
        }
        active_types.emplace(key, std::move(candidate));
    }
    if (traceEnabled()) {
        (void)std::fprintf(
            stderr,
            "[cvite] registered %llu managed baseline types\n",
            static_cast<unsigned long long>(manifest->type_count));
    }
    if (message != nullptr && message_size > 0U) {
        message[0] = '\0';
    }
    return 1;
}

extern "C" int __cvite_host_prepare_managed_type_manifest(
    const cvite_managed_type_manifest *manifest,
    char *message,
    std::size_t message_size)
{
    std::string error;
    if (!validateManifest(manifest, error)) {
        writeMessage(message, message_size, error);
        return 0;
    }
    if (manifest == nullptr) {
        return 1;
    }

    std::lock_guard<std::mutex> lock(registry_mutex);
    std::vector<CandidateType> candidates;
    candidates.reserve(static_cast<std::size_t>(manifest->type_count));
    bool needs_writer = false;
    for (std::uint64_t index = 0U; index < manifest->type_count; ++index) {
        CandidateType candidate;
        if (!copyType(manifest->types[index], candidate.owned, error)) {
            writeMessage(message, message_size, error);
            return 0;
        }
        candidate.key = {candidate.owned.id_high, candidate.owned.id_low};
        const auto found = active_types.find(candidate.key);
        if (found == active_types.end()) {
            candidate.plan.compatibility =
                cvite::refresh::ManagedTypeCompatibility::identical;
            candidate.plan.diagnostic =
                candidate.owned.name + ": new managed type";
            candidates.push_back(std::move(candidate));
            continue;
        }
        candidate.existed = true;
        const auto old_views = found->second.fieldViews();
        const auto new_views = candidate.owned.fieldViews();
        const auto old_view = found->second.view(old_views);
        const auto new_view = candidate.owned.view(new_views);
        candidate.plan = cvite::refresh::planManagedTypeEvolution(
            old_view, new_view);
        if (candidate.plan.compatibility ==
            cvite::refresh::ManagedTypeCompatibility::incompatible) {
            writeMessage(message, message_size, candidate.plan.diagnostic);
            return 0;
        }
        needs_writer = needs_writer ||
            candidate.plan.compatibility ==
                cvite::refresh::ManagedTypeCompatibility::append_only;
        candidates.push_back(std::move(candidate));
    }

    if (needs_writer && !__cvite_host_managed_begin_write()) {
        writeMessage(
            message,
            message_size,
            "cannot acquire managed refresh writer gate");
        return 0;
    }

    bool success = true;
    std::string failure;
    cvite_managed_domain *domain = __cvite_host_managed_domain_instance();
    for (CandidateType &candidate : candidates) {
        if (candidate.plan.compatibility !=
            cvite::refresh::ManagedTypeCompatibility::append_only) {
            continue;
        }
        cvite_managed_error managed_error;
        cvite_managed_error_clear(&managed_error);
        std::size_t migrated = 0U;
        const cvite_managed_status status = cvite_managed_migrate_type(
            domain,
            (cvite_id){candidate.owned.id_high, candidate.owned.id_low},
            candidate.plan.byte_plan.new_size,
            candidate.plan.byte_plan.alignment,
            cvite_managed_apply_byte_plan,
            &candidate.plan.byte_plan,
            &migrated,
            &managed_error);
        if (status != CVITE_MANAGED_OK) {
            failure = candidate.plan.diagnostic + ": " +
                cvite_managed_status_name(status);
            if (managed_error.message[0] != '\0') {
                failure += ": ";
                failure += managed_error.message;
            }
            success = false;
            break;
        }
        if (traceEnabled()) {
            (void)std::fprintf(
                stderr,
                "[cvite] %s; migrated %zu objects\n",
                candidate.plan.diagnostic.c_str(),
                migrated);
        }
        active_types[candidate.key] = candidate.owned;
    }

    if (needs_writer) {
        __cvite_host_managed_end_write();
    }
    if (!success) {
        writeMessage(message, message_size, failure);
        return 0;
    }
    for (CandidateType &candidate : candidates) {
        if (candidate.plan.compatibility !=
            cvite::refresh::ManagedTypeCompatibility::append_only) {
            active_types[candidate.key] = std::move(candidate.owned);
        }
    }
    if (message != nullptr && message_size > 0U) {
        message[0] = '\0';
    }
    return 1;
}
