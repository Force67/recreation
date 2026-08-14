#ifndef RECREATION_WORLD_ACTOR_STATS_STORE_H_
#define RECREATION_WORLD_ACTOR_STATS_STORE_H_

// Actor level and temperament per NPC base form: what the records author, with
// a resumed savegame's overrides laid over the top.
//
// It is a store of its own rather than streamer state because the two happen
// hours apart in bring-up terms: a save is applied before the cell streamer
// exists, and the actor it describes may not stream in for another ten minutes
// of play. Whoever creates the actor asks here.

#include <base/containers/unordered_map.h>

#include "components/bethesda/actor_stats.h"
#include "components/bethesda/form_id.h"
#include "components/bethesda/record.h"
#include "core/types.h"

namespace rx::world {

class ActorStatsStore {
 public:
  void OverrideLevel(bethesda::GlobalFormId base, u32 level) {
    Entry& entry = overrides_[base.packed()];
    entry.has_level = true;
    entry.level = level;
  }

  void OverrideAi(bethesda::GlobalFormId base, const bethesda::ActorAi& ai) {
    Entry& entry = overrides_[base.packed()];
    entry.has_ai = true;
    entry.ai = ai;
  }

  // Every level-mult actor scales against this, so it has to be set before the
  // first actor is resolved or they all come up at their floor level.
  void set_player_level(u32 level) { player_level_ = level; }
  u32 player_level() const { return player_level_; }
  size_t overrides() const { return overrides_.size(); }

  // The level an encounter zone locked to the first time the player walked into
  // it. Everything the zone holds scales against that instead of the player, so
  // a dungeon done at level 6 is still a level 6 dungeon when a level 271
  // character walks back in.
  void SetZoneLevel(bethesda::GlobalFormId zone, u32 level) { zones_[zone.packed()] = level; }
  // Zero when the zone never locked, which means "scale against the player".
  u32 ZoneLevel(bethesda::GlobalFormId zone) const {
    const u32* level = zones_.find(zone.packed());
    return level ? *level : 0;
  }
  size_t zones() const { return zones_.size(); }

  // Stats for one placed actor, from its base NPC_ record and any override.
  // `zone` is the encounter zone the actor stands in, invalid or unknown for
  // one that stands in none.
  // A default-constructed id is already plugin 0xffff, so omitting the zone
  // says "stands in none" rather than needing a spelling of its own.
  bethesda::ActorStats For(bethesda::GlobalFormId base,
                           const bethesda::Record& base_record,
                           bethesda::GlobalFormId zone = {}) const {
    const u32 locked = zone.plugin == 0xffff ? 0 : ZoneLevel(zone);
    bethesda::ActorStats stats;
    bethesda::ReadActorStats(base_record, locked != 0 ? locked : player_level_, &stats);
    const Entry* entry = overrides_.find(base.packed());
    if (!entry)
      return stats;
    if (entry->has_level)
      stats.level = entry->level;
    if (entry->has_ai) {
      stats.has_ai = true;
      stats.ai = entry->ai;
    }
    return stats;
  }

  void Clear() {
    overrides_.clear();
    zones_.clear();
  }

 private:
  // The two groups are written independently by a savegame, so each says for
  // itself whether it is there.
  struct Entry {
    bool has_level = false;
    bool has_ai = false;
    u32 level = 1;
    bethesda::ActorAi ai;
  };

  base::UnorderedMap<u64, Entry> overrides_;
  base::UnorderedMap<u64, u32> zones_;  // ECZN -> the level it locked to
  u32 player_level_ = 1;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_ACTOR_STATS_STORE_H_
