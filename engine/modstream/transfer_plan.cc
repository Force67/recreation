#include "modstream/transfer_plan.h"

#include <unordered_set>

namespace rx::modstream {

std::vector<NeededFile> ComputeMissing(const ModManifest& manifest,
                                       const ContentStore& store) {
  std::vector<NeededFile> plan;
  std::unordered_set<ContentHash> seen;
  for (const ModResource& resource : manifest.resources) {
    for (const ResourceFile& file : resource.files) {
      if (!seen.insert(file.hash).second) continue;  // already planned
      if (store.Has(file.hash)) continue;            // already cached
      plan.push_back({file.hash, file.size});
    }
  }
  return plan;
}

u64 PlannedBytes(const std::vector<NeededFile>& plan) {
  u64 total = 0;
  for (const NeededFile& file : plan) total += file.size;
  return total;
}

}  // namespace rx::modstream
