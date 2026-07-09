#include "modstream/mod_resource.h"

namespace rx::modstream {

u64 ModManifest::TotalBytes() const {
  u64 total = 0;
  for (const ModResource& resource : resources) {
    for (const ResourceFile& file : resource.files) total += file.size;
  }
  return total;
}

size_t ModManifest::TotalFiles() const {
  size_t total = 0;
  for (const ModResource& resource : resources) total += resource.files.size();
  return total;
}

}  // namespace rx::modstream
