#include "components/world/misc_stats.h"

namespace rx::world {

void MiscStats::Set(base::StringRef name, u8 category, u32 value) {
  base::String key(name.data(), name.size());
  if (const u32* slot = index_.find(key)) {
    stats_[*slot].category = category;
    stats_[*slot].value = value;
    return;
  }
  index_[key] = static_cast<u32>(stats_.size());
  MiscStat stat;
  stat.name = base::move(key);
  stat.category = category;
  stat.value = value;
  stats_.push_back(base::move(stat));
}

u32 MiscStats::Value(base::StringRef name) const {
  const u32* slot = index_.find(base::String(name.data(), name.size()));
  return slot ? stats_[*slot].value : 0;
}

bool MiscStats::Has(base::StringRef name) const {
  return index_.find(base::String(name.data(), name.size())) != nullptr;
}

void MiscStats::Clear() {
  stats_.clear();
  index_.clear();
}

}  // namespace rx::world
