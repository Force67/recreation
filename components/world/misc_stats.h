#ifndef RECREATION_WORLD_MISC_STATS_H_
#define RECREATION_WORLD_MISC_STATS_H_

// The game's Stats page: every "X killed", "Y picked", locations discovered,
// days passed, lifetime bounty.
//
// A savegame stores these as a flat list of (name, category, value), where the
// name is the game's own untranslated key and the only label there is. Nothing
// derives them: they are counters play increments, so a resumed save is the
// only thing that can put a number in here that the session did not.
//
// Kept as a store rather than pushed straight at a menu because two things want
// them for different reasons: the Stats screen shows the whole list, and the
// crime/faction code wants single rows by name.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::world {

struct MiscStat {
  base::String name;
  u8 category = 0;  // the game's own Stats tab index; see MiscStats::Set
  u32 value = 0;
};

class MiscStats {
 public:
  // Last writer wins, which is what a save being applied over a session wants.
  // `category` is the game's tab index and means nothing across games (Skyrim
  // numbers them 0..6, Fallout 4 uses 0..5 and 7), so it is carried, not read.
  void Set(base::StringRef name, u8 category, u32 value);

  // 0 for a stat the game never wrote. A counter that has not happened and one
  // the game does not have both read zero, and the file cannot tell them apart.
  u32 Value(base::StringRef name) const;
  bool Has(base::StringRef name) const;

  // In insertion order, which for a save is the game's own order within each
  // category. Stable, so a menu can page through it.
  const base::Vector<MiscStat>& all() const { return stats_; }
  mem_size size() const { return stats_.size(); }
  bool empty() const { return stats_.empty(); }
  void Clear();

 private:
  base::Vector<MiscStat> stats_;
  base::UnorderedMap<base::String, u32> index_;  // name -> slot in stats_
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_MISC_STATS_H_
