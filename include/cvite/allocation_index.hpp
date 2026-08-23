#ifndef CVITE_ALLOCATION_INDEX_HPP
#define CVITE_ALLOCATION_INDEX_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cvite::semantic {

enum class AllocationKind {
    malloc_call,
    calloc_call,
    realloc_call,
    aligned_alloc_call,
};

enum class AllocationConfidence {
    unknown,
    typed_result,
    sizeof_type,
};

struct AllocationSite final {
    std::string id;
    std::string source;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    AllocationKind kind = AllocationKind::malloc_call;
    AllocationConfidence confidence = AllocationConfidence::unknown;
    std::string type_id;
    std::string type_name;
};

struct AllocationIndex final {
    std::string target;
    std::vector<AllocationSite> sites;
};

bool buildAllocationIndex(
    const std::string &source,
    const std::vector<std::string> &arguments,
    AllocationIndex &index,
    std::string &diagnostics);

bool writeAllocationIndex(
    const AllocationIndex &index,
    const std::string &path,
    std::string &error);

bool readAllocationIndex(
    const std::string &path,
    AllocationIndex &index,
    std::string &error);

std::string formatAllocationSite(const AllocationSite &site);
const char *allocationKindName(AllocationKind kind);
const char *allocationConfidenceName(AllocationConfidence confidence);

} // namespace cvite::semantic

#endif
