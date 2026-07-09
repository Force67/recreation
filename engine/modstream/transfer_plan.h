#ifndef RECREATION_MODSTREAM_TRANSFER_PLAN_H_
#define RECREATION_MODSTREAM_TRANSFER_PLAN_H_

#include <vector>

#include "modstream/content_store.h"
#include "modstream/mod_resource.h"

namespace rx::modstream {

// One piece of content a client still has to fetch.
struct NeededFile {
  ContentHash hash = 0;
  u64 size = 0;
};

// The selective part of asset streaming: walks the server manifest and returns
// only the content the cache is missing, deduped by hash so a file shared by
// several resources is fetched once. Order is the manifest's, by first
// appearance, so the plan is deterministic. An up-to-date client gets an empty
// plan and downloads nothing.
std::vector<NeededFile> ComputeMissing(const ModManifest& manifest,
                                       const ContentStore& store);

// Total bytes a plan will transfer, for progress reporting and join budgeting.
u64 PlannedBytes(const std::vector<NeededFile>& plan);

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_TRANSFER_PLAN_H_
