// Authored faction-membership accessors for RecordBackedSkyrimBindings. An NPC's
// factions live in its NPC_ record's SNAM entries; the runtime faction store (in
// skyrim_bindings.cc) overrides them. Reading the record here lets GetFactionRank
// fall back to authored membership and lets managed code enumerate an actor's
// factions. Split out to keep skyrim_bindings.cc from becoming a god file.
#include <cstring>
#include <utility>
#include <vector>

#include "bethesda/record.h"
#include "script/games/skyrim/skyrim_bindings.h"

namespace rec::script::skyrim {
namespace {

using papyrus::ObjectRef;

// Resolves `id` to its NPC_ record: the record itself if it is one, else the base
// its NAME points at (a placed actor reference). Returns false if there is no NPC_
// behind it. On success `out` holds the NPC_ record and `plugin` its winning
// plugin (for resolving the SNAM faction ids).
bool ResolveNpc(const bethesda::RecordStore* records, bethesda::GlobalFormId id,
                bethesda::Record* out, u16* plugin) {
  const bethesda::RecordStore::StoredRecord* stored = records->Find(id);
  if (!stored || !records->Parse(id, out)) return false;
  if (out->header.type == FourCc('N', 'P', 'C', '_')) {
    *plugin = stored->winning_plugin;
    return true;
  }
  const bethesda::Subrecord* name = out->Find(FourCc('N', 'A', 'M', 'E'));
  if (!name || name->data.size() < 4) return false;
  u32 raw;
  std::memcpy(&raw, name->data.data(), 4);
  const bethesda::GlobalFormId base = records->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
  const bethesda::RecordStore::StoredRecord* bstored = records->Find(base);
  if (!bstored || !records->Parse(base, out) || out->header.type != FourCc('N', 'P', 'C', '_'))
    return false;
  *plugin = bstored->winning_plugin;
  return true;
}

// Reads the NPC_'s SNAM faction entries as (faction handle, rank). SNAM is 8
// bytes: { formid faction; int8 rank; uint8[3] unused }.
void ReadFactions(const bethesda::RecordStore* records, const bethesda::Record& npc, u16 plugin,
                  std::vector<std::pair<u64, i32>>* out) {
  for (const bethesda::Subrecord& sub : npc.subrecords) {
    if (sub.type != FourCc('S', 'N', 'A', 'M') || sub.data.size() < 5) continue;
    u32 raw;
    std::memcpy(&raw, sub.data.data(), 4);
    const i8 rank = static_cast<i8>(sub.data[4]);
    out->push_back({records->ResolveFrom(bethesda::RawFormId{raw}, plugin).packed(), rank});
  }
}

}  // namespace

i32 RecordBackedSkyrimBindings::GetFactionCount(ObjectRef actor) {
  faction_cache_.clear();
  if (!records_) return 0;
  bethesda::Record npc;
  u16 plugin;
  if (!ResolveNpc(records_, ToFormId(actor), &npc, &plugin)) return 0;
  ReadFactions(records_, npc, plugin, &faction_cache_);
  return static_cast<i32>(faction_cache_.size());
}

ObjectRef RecordBackedSkyrimBindings::GetNthFaction(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= faction_cache_.size()) return {};
  return ObjectRef{faction_cache_[static_cast<size_t>(index)].first};
}

i32 RecordBackedSkyrimBindings::GetNthFactionRank(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= faction_cache_.size()) return 0;
  return faction_cache_[static_cast<size_t>(index)].second;
}

i32 RecordBackedSkyrimBindings::AuthoredFactionRank(ObjectRef actor, ObjectRef faction) {
  if (!records_) return -2;
  bethesda::Record npc;
  u16 plugin;
  if (!ResolveNpc(records_, ToFormId(actor), &npc, &plugin)) return -2;
  // A local list so a query during enumeration does not clobber faction_cache_.
  std::vector<std::pair<u64, i32>> factions;
  ReadFactions(records_, npc, plugin, &factions);
  for (const auto& [handle, rank] : factions)
    if (handle == faction.handle) return rank;
  return -2;
}

i32 RecordBackedSkyrimBindings::AuthoredFactionReaction(ObjectRef faction, ObjectRef other) {
  if (!records_) return 0;  // 0 = neutral
  const bethesda::GlobalFormId id = ToFormId(faction);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec) || rec.header.type != FourCc('F', 'A', 'C', 'T')) return 0;
  // FACT XNAM is 12 bytes: { formid faction; int32 modifier; uint32 reaction }.
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type != FourCc('X', 'N', 'A', 'M') || sub.data.size() < 12) continue;
    u32 raw;
    std::memcpy(&raw, sub.data.data(), 4);
    if (records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed() != other.handle)
      continue;
    u32 reaction;
    std::memcpy(&reaction, sub.data.data() + 8, 4);
    return static_cast<i32>(reaction);
  }
  return 0;
}

}  // namespace rec::script::skyrim
