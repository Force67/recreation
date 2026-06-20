// Leveled item list (LVLI) accessors for RecordBackedSkyrimBindings. A leveled
// list is the data behind Skyrim's loot, drops and vendor stock: a set of entries
// gated by player level, resolved at runtime to concrete items. The parsing lives
// here (the Skyrim impl detail); the managed resolver does the level-gating and
// random picks. Split from skyrim_bindings.cc to keep that unit small.
#include <cstring>

#include "bethesda/record.h"
#include "script/games/skyrim/skyrim_bindings.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;

i32 RecordBackedSkyrimBindings::GetLeveledListCount(ObjectRef list) {
  leveled_cache_ = LeveledList{};
  if (!records_) return 0;
  const bethesda::GlobalFormId id = ToFormId(list);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return 0;
  if (rec.header.type != FourCc('L', 'V', 'L', 'I')) return 0;

  // LVLD is the percent chance of nothing, LVLF the flags, and each LVLO is a
  // 12-byte entry { uint16 level; uint16 pad; uint32 reference; uint16 count }.
  // Entry form ids are local to the list's plugin, so resolve them.
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type == FourCc('L', 'V', 'L', 'D') && !sub.data.empty()) {
      leveled_cache_.chance_none = sub.data[0];
    } else if (sub.type == FourCc('L', 'V', 'L', 'F') && !sub.data.empty()) {
      leveled_cache_.flags = sub.data[0];
    } else if (sub.type == FourCc('L', 'V', 'L', 'O') && sub.data.size() >= 12) {
      u16 level, count;
      u32 raw;
      std::memcpy(&level, sub.data.data(), 2);
      std::memcpy(&raw, sub.data.data() + 4, 4);
      std::memcpy(&count, sub.data.data() + 8, 2);
      leveled_cache_.entries.push_back(
          {records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed(), level,
           count});
    }
  }
  return static_cast<i32>(leveled_cache_.entries.size());
}

i32 RecordBackedSkyrimBindings::GetLeveledChanceNone() { return leveled_cache_.chance_none; }
i32 RecordBackedSkyrimBindings::GetLeveledFlags() { return leveled_cache_.flags; }

ObjectRef RecordBackedSkyrimBindings::GetNthLeveledForm(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= leveled_cache_.entries.size()) return {};
  return ObjectRef{leveled_cache_.entries[static_cast<size_t>(index)].form};
}

i32 RecordBackedSkyrimBindings::GetNthLeveledLevel(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= leveled_cache_.entries.size()) return 0;
  return leveled_cache_.entries[static_cast<size_t>(index)].level;
}

i32 RecordBackedSkyrimBindings::GetNthLeveledCount(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= leveled_cache_.entries.size()) return 0;
  return leveled_cache_.entries[static_cast<size_t>(index)].count;
}

}  // namespace rec::script::skyrim
