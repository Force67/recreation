#include "script/games/skyrim/skyrim_bindings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "bethesda/record.h"
#include "core/log.h"
#include "quest/quest_def.h"
#include "quest/quest_import.h"
#include "script/papyrus/alias_handle.h"
#include "script/papyrus/vm.h"

namespace rec::script::skyrim {
namespace {

using papyrus::ObjectRef;

// Seconds the ScenePlayer dwells in each scene phase before advancing. Scenes
// really pace on dialogue length / completion conditions; a fixed cadence keeps
// the journal moving until that runtime exists.
constexpr f32 kScenePhaseSeconds = 2.0f;

// Bridges ScenePlayer cues to the scene fragment runners on the bindings.
struct SceneCueSink : quest::ScenePlayerSink {
  RecordBackedSkyrimBindings* b = nullptr;
  void SceneBegin(u64 scene) override { b->RunSceneBegin(scene); }
  void ScenePhase(u64 scene, u32 phase, bool on_begin) override {
    b->RunScenePhase(scene, phase, on_begin);
  }
  void SceneEnd(u64 scene) override { b->RunSceneEnd(scene); }
};

std::string Lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
  return s;
}

// Record signature -> Papyrus Form.GetType() value (the standard FormType enum).
// A common subset; unmapped records report 0 (None).
i32 FormTypeFromSignature(u32 type) {
  switch (type) {
    case FourCc('K', 'Y', 'W', 'D'): return 4;
    case FourCc('F', 'A', 'C', 'T'): return 11;
    case FourCc('R', 'A', 'C', 'E'): return 15;
    case FourCc('M', 'G', 'E', 'F'): return 19;
    case FourCc('S', 'P', 'E', 'L'): return 23;
    case FourCc('S', 'C', 'R', 'L'): return 24;
    case FourCc('A', 'C', 'T', 'I'): return 25;
    case FourCc('A', 'R', 'M', 'O'): return 27;
    case FourCc('B', 'O', 'O', 'K'): return 28;
    case FourCc('C', 'O', 'N', 'T'): return 29;
    case FourCc('D', 'O', 'O', 'R'): return 30;
    case FourCc('I', 'N', 'G', 'R'): return 31;
    case FourCc('L', 'I', 'G', 'H'): return 32;
    case FourCc('M', 'I', 'S', 'C'): return 33;
    case FourCc('S', 'T', 'A', 'T'): return 35;
    case FourCc('F', 'L', 'O', 'R'): return 40;
    case FourCc('F', 'U', 'R', 'N'): return 41;
    case FourCc('W', 'E', 'A', 'P'): return 42;
    case FourCc('A', 'M', 'M', 'O'): return 43;
    case FourCc('N', 'P', 'C', '_'): return 44;
    case FourCc('K', 'E', 'Y', 'M'): return 46;
    case FourCc('A', 'L', 'C', 'H'): return 47;
    case FourCc('S', 'L', 'G', 'M'): return 53;
    case FourCc('W', 'T', 'H', 'R'): return 55;
    case FourCc('C', 'E', 'L', 'L'): return 61;
    case FourCc('R', 'E', 'F', 'R'): return 62;
    case FourCc('A', 'C', 'H', 'R'): return 63;
    case FourCc('Q', 'U', 'S', 'T'): return 78;
    default: return 0;
  }
}

f32 DefaultActorValue(const std::string& av) {
  std::string a = Lower(av);
  if (a == "health" || a == "magicka" || a == "stamina") return 100.0f;
  return 0.0f;
}

// The NPC_ ACBS flags word (bit 0 Female, bit 1 Essential, bit 5 Unique).
u32 AcbsFlags(const bethesda::RecordStore* records, bethesda::GlobalFormId id) {
  if (!records) return 0;
  bethesda::Record rec;
  if (!records->Parse(id, &rec)) return 0;
  const bethesda::Subrecord* acbs = rec.Find(FourCc('A', 'C', 'B', 'S'));
  if (!acbs || acbs->data.size() < 4) return 0;
  u32 flags;
  std::memcpy(&flags, acbs->data.data(), 4);
  return flags;
}

}  // namespace

bethesda::GlobalFormId RecordBackedSkyrimBindings::ToFormId(ObjectRef ref) const {
  return bethesda::GlobalFormId{static_cast<u16>(ref.handle >> 32),
                                static_cast<u32>(ref.handle)};
}

u32 RecordBackedSkyrimBindings::GetFormId(ObjectRef form) {
  return static_cast<u32>(form.handle);
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetForm(u32 form_id) {
  if (!records_) return {};
  // Runtime form id: high byte is the load-order index, low 24 bits the local
  // id. (Light/ESL forms 0xFE... are not resolved here and yield None.)
  bethesda::GlobalFormId id{static_cast<u16>((form_id >> 24) & 0xFF), form_id & 0x00FFFFFFu};
  if (!records_->Find(id)) return {};
  return papyrus::ObjectRef{id.packed()};
}

void RecordBackedSkyrimBindings::AliasForceRefTo(ObjectRef alias, ObjectRef ref) {
  if (replica_mode_ || !papyrus::IsAliasHandle(alias.handle)) return;
  if (ref.handle == 0)
    alias_fills_.erase(alias.handle);
  else
    alias_fills_[alias.handle] = ref.handle;
}

void RecordBackedSkyrimBindings::AliasClear(ObjectRef alias) {
  if (replica_mode_) return;
  alias_fills_.erase(alias.handle);
}

papyrus::ObjectRef RecordBackedSkyrimBindings::AliasReference(ObjectRef alias) {
  if (!papyrus::IsAliasHandle(alias.handle)) return {};
  // A runtime fill (ForceRefTo) wins over the authored rule; it was a valid ref
  // when set, so it is returned without re-validating against records.
  if (const auto it = alias_fills_.find(alias.handle); it != alias_fills_.end())
    return papyrus::ObjectRef{it->second};
  if (!records_) return {};
  const u64 quest = papyrus::AliasHandleQuest(alias.handle);
  const i32 alias_id = static_cast<i32>(papyrus::AliasHandleAliasId(alias.handle));
  const quest::QuestDef* def = quest_system_.Definition(quest);
  if (!def) return {};
  const quest::AliasDef* a = def->FindAlias(alias_id);
  if (!a) return {};
  // Alias form ids resolve against the quest record's plugin.
  const bethesda::GlobalFormId quest_id{static_cast<u16>(quest >> 32),
                                        static_cast<u32>(quest & 0xffffffffu)};
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(quest_id);
  const u16 plugin = stored ? stored->winning_plugin : static_cast<u16>(quest >> 32);

  bethesda::GlobalFormId ref;
  if (a->forced_ref_raw != 0) {
    // A forced reference (ALFR) names the placed ref directly.
    ref = records_->ResolveFrom(bethesda::RawFormId{a->forced_ref_raw}, plugin);
  } else if (a->unique_actor_raw != 0) {
    // A unique-actor alias (ALUA) is filled with that NPC's placed ACHR.
    const bethesda::GlobalFormId base =
        records_->ResolveFrom(bethesda::RawFormId{a->unique_actor_raw}, plugin);
    ref = records_->PlacedRefForBase(base);
  } else {
    return {};
  }
  if (ref.plugin == 0xffff || !records_->Find(ref)) return {};
  return papyrus::ObjectRef{ref.packed()};
}

i32 RecordBackedSkyrimBindings::GetFormType(ObjectRef form) {
  if (!records_) return 0;
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(ToFormId(form));
  return stored ? FormTypeFromSignature(stored->header.type) : 0;
}

// Item record-data accessors (GetWeight, GetGoldValue, GetWeaponDamage,
// GetArmorRating) live in skyrim_bindings_item_data.cc.

std::string RecordBackedSkyrimBindings::GetName(ObjectRef form) {
  if (!records_) return "";
  bethesda::Record record;
  if (!records_->Parse(ToFormId(form), &record)) return "";
  const bethesda::Subrecord* full = record.Find(FourCc('F', 'U', 'L', 'L'));
  if (!full) {
    // A placed reference (REFR/ACHR) carries no FULL of its own; the displayed
    // name lives on its base object, reached through NAME.
    ObjectRef base = GetBaseObject(form);
    return base.handle != form.handle && base.handle != 0 ? GetName(base) : "";
  }
  if (strings_ && full->data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, full->data.data(), 4);
    if (const base::String* s = strings_->Find(string_id)) return std::string(s->c_str());
  }
  return record.GetString(FourCc('F', 'U', 'L', 'L'));  // non-localized inline text
}

bool RecordBackedSkyrimBindings::HasKeyword(ObjectRef form, ObjectRef keyword) {
  if (!records_) return false;
  bethesda::Record record;
  if (!records_->Parse(ToFormId(form), &record)) return false;
  const bethesda::Subrecord* kwda = record.Find(FourCc('K', 'W', 'D', 'A'));
  if (!kwda) return false;
  u32 want = static_cast<u32>(keyword.handle);
  for (size_t offset = 0; offset + 4 <= kwda->data.size(); offset += 4) {
    u32 id;
    std::memcpy(&id, kwda->data.data() + offset, 4);
    if (id == want) return true;
  }
  return false;
}

std::array<f32, 3> RecordBackedSkyrimBindings::Position(ObjectRef ref) {
  auto it = positions_.find(ref.handle);
  if (it != positions_.end()) return it->second;
  // Fall back to the authored placement in the REFR record (DATA = pos+rot).
  if (records_) {
    bethesda::Record record;
    if (records_->Parse(ToFormId(ref), &record)) {
      const bethesda::Subrecord* data = record.Find(FourCc('D', 'A', 'T', 'A'));
      if (data && data->data.size() >= 12) {
        std::array<f32, 3> p{};
        std::memcpy(p.data(), data->data.data(), 12);
        return p;
      }
    }
  }
  return {0.0f, 0.0f, 0.0f};
}

f32 RecordBackedSkyrimBindings::GetPositionX(ObjectRef ref) { return Position(ref)[0]; }
f32 RecordBackedSkyrimBindings::GetPositionY(ObjectRef ref) { return Position(ref)[1]; }
f32 RecordBackedSkyrimBindings::GetPositionZ(ObjectRef ref) { return Position(ref)[2]; }

void RecordBackedSkyrimBindings::SetPosition(ObjectRef ref, f32 x, f32 y, f32 z) {
  if (replica_mode_) return;  // server-authoritative: positions arrive replicated
  positions_[ref.handle] = {x, y, z};  // logical position for script reads
  if (!world_sink_) return;
  if (ref.handle == player_.handle)
    world_sink_->MovePlayer(active_quest_, 0, x, y, z);  // raw coords, no dest ref
  else
    world_sink_->MoveReference(active_quest_, ref.handle, x, y, z);
}

f32 RecordBackedSkyrimBindings::GetDistance(ObjectRef a, ObjectRef b) {
  std::array<f32, 3> pa = Position(a);
  std::array<f32, 3> pb = Position(b);
  f32 dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void RecordBackedSkyrimBindings::MoveTo(ObjectRef ref, ObjectRef target) {
  if (replica_mode_) return;
  std::array<f32, 3> p = Position(target);
  positions_[ref.handle] = p;
  if (!world_sink_) return;
  if (ref.handle == player_.handle)
    world_sink_->MovePlayer(active_quest_, target.handle, p[0], p[1], p[2]);
  else
    world_sink_->MoveReference(active_quest_, ref.handle, p[0], p[1], p[2]);
}

void RecordBackedSkyrimBindings::SetEnabled(ObjectRef ref, bool enabled) {
  if (replica_mode_) return;
  disabled_[ref.handle] = !enabled;
  if (world_sink_) world_sink_->SetEnabled(active_quest_, ref.handle, enabled);
}

bool RecordBackedSkyrimBindings::IsDisabled(ObjectRef ref) {
  auto it = disabled_.find(ref.handle);
  return it != disabled_.end() && it->second;
}

void RecordBackedSkyrimBindings::Delete(ObjectRef ref) {
  if (replica_mode_) return;
  if (world_sink_) world_sink_->DeleteReference(active_quest_, ref.handle);
}

papyrus::ObjectRef RecordBackedSkyrimBindings::PlaceAtMe(ObjectRef where, ObjectRef base,
                                                         i32 /*count*/) {
  if (replica_mode_ || !world_sink_) return ObjectRef{0};
  // Spawn at the placer's position; the sink allocates the new ref handle so we
  // can return it to the script immediately. Mirror the position locally so the
  // script's GetPosition on the new ref reads back.
  std::array<f32, 3> p = Position(where);
  u64 handle = world_sink_->SpawnReference(active_quest_, base.handle, p[0], p[1], p[2]);
  positions_[handle] = p;
  return ObjectRef{handle};
}

f32 RecordBackedSkyrimBindings::GetScale(ObjectRef ref) {
  auto it = scales_.find(ref.handle);
  return it == scales_.end() ? 1.0f : it->second;
}

void RecordBackedSkyrimBindings::SetScale(ObjectRef ref, f32 scale) {
  scales_[ref.handle] = scale;
}

bool RecordBackedSkyrimBindings::IsLocked(ObjectRef ref) {
  auto it = locks_.find(ref.handle);
  return it != locks_.end() && it->second.locked;
}

void RecordBackedSkyrimBindings::SetLocked(ObjectRef ref, bool locked) {
  locks_[ref.handle].locked = locked;
}

i32 RecordBackedSkyrimBindings::GetLockLevel(ObjectRef ref) {
  auto it = locks_.find(ref.handle);
  return it == locks_.end() ? 0 : it->second.level;
}

void RecordBackedSkyrimBindings::SetLockLevel(ObjectRef ref, i32 level) {
  locks_[ref.handle].level = level;
}

i32 RecordBackedSkyrimBindings::GetOpenState(ObjectRef ref) {
  auto it = open_.find(ref.handle);
  if (it == open_.end()) return 3;  // closed
  return it->second ? 1 : 3;
}

void RecordBackedSkyrimBindings::SetOpen(ObjectRef ref, bool open) { open_[ref.handle] = open; }

i32 RecordBackedSkyrimBindings::GetFactionRank(ObjectRef actor, ObjectRef faction) {
  // A runtime override (Add/Remove/SetFactionRank) wins; otherwise fall back to
  // the membership authored in the NPC_ record. A removed faction is stored as the
  // -2 sentinel so the override can hide an authored membership.
  auto it = faction_ranks_.find(actor.handle);
  if (it != faction_ranks_.end()) {
    auto rank_it = it->second.find(faction.handle);
    if (rank_it != it->second.end()) return rank_it->second;
  }
  return AuthoredFactionRank(actor, faction);
}

void RecordBackedSkyrimBindings::SetFactionRank(ObjectRef actor, ObjectRef faction, i32 rank) {
  faction_ranks_[actor.handle][faction.handle] = rank;
}

bool RecordBackedSkyrimBindings::IsInFaction(ObjectRef actor, ObjectRef faction) {
  return GetFactionRank(actor, faction) > -2;  // -2 means not a member
}

void RecordBackedSkyrimBindings::AddToFaction(ObjectRef actor, ObjectRef faction) {
  faction_ranks_[actor.handle].try_emplace(faction.handle, 0);
}

void RecordBackedSkyrimBindings::RemoveFromFaction(ObjectRef actor, ObjectRef faction) {
  // Store the -2 sentinel rather than erase, so the removal also hides an authored
  // membership the NPC_ record would otherwise report.
  faction_ranks_[actor.handle][faction.handle] = -2;
}

i32 RecordBackedSkyrimBindings::GetReaction(ObjectRef faction, ObjectRef other) {
  // A runtime override (SetReaction) wins; otherwise fall back to the reaction
  // authored in the FACT record.
  auto it = reactions_.find(faction.handle);
  if (it != reactions_.end()) {
    auto other_it = it->second.find(other.handle);
    if (other_it != it->second.end()) return other_it->second;
  }
  return AuthoredFactionReaction(faction, other);
}

void RecordBackedSkyrimBindings::SetReaction(ObjectRef faction, ObjectRef other, i32 reaction) {
  reactions_[faction.handle][other.handle] = reaction;
}

i32 RecordBackedSkyrimBindings::GetCrimeGold(ObjectRef faction) {
  auto it = crime_gold_.find(faction.handle);
  return it == crime_gold_.end() ? 0 : it->second;
}

void RecordBackedSkyrimBindings::SetCrimeGold(ObjectRef faction, i32 gold) {
  crime_gold_[faction.handle] = std::max(0, gold);
}

void RecordBackedSkyrimBindings::ModCrimeGold(ObjectRef faction, i32 delta) {
  i32& gold = crime_gold_[faction.handle];
  gold = std::max(0, gold + delta);
}

void RecordBackedSkyrimBindings::SetPlayerControl(i32 category, bool enabled) {
  if (!player_controls_init_) {
    player_controls_.fill(true);
    player_controls_init_ = true;
  }
  if (category >= 0 && category < kControlCount) player_controls_[category] = enabled;
}

bool RecordBackedSkyrimBindings::IsPlayerControlEnabled(i32 category) {
  if (!player_controls_init_) return true;  // all enabled until something toggles
  return category >= 0 && category < kControlCount ? player_controls_[category] : true;
}

f32 RecordBackedSkyrimBindings::GetGlobalValue(ObjectRef global) {
  auto it = global_values_.find(global.handle);
  if (it != global_values_.end()) return it->second;
  // Fall back to the GLOB record's authored value (FLTV).
  if (records_) {
    bethesda::Record rec;
    if (records_->Parse(ToFormId(global), &rec)) {
      const bethesda::Subrecord* fltv = rec.Find(FourCc('F', 'L', 'T', 'V'));
      if (fltv && fltv->data.size() >= 4) {
        f32 value;
        std::memcpy(&value, fltv->data.data(), 4);
        return value;
      }
    }
  }
  return 0.0f;
}

void RecordBackedSkyrimBindings::SetGlobalValue(ObjectRef global, f32 value) {
  global_values_[global.handle] = value;
}

RecordBackedSkyrimBindings::ActorValue& RecordBackedSkyrimBindings::Av(ObjectRef actor,
                                                                       const std::string& av) {
  auto& values = actor_values_[actor.handle];
  std::string key = Lower(av);
  auto it = values.find(key);
  if (it != values.end()) return it->second;
  f32 d = DefaultActorValue(av);
  return values[key] = ActorValue{d, d};
}

i32 RecordBackedSkyrimBindings::GetSex(ObjectRef actor_base) {
  return (AcbsFlags(records_, ToFormId(actor_base)) & 0x1) ? 1 : 0;
}

bool RecordBackedSkyrimBindings::IsUnique(ObjectRef actor_base) {
  return (AcbsFlags(records_, ToFormId(actor_base)) & 0x20) != 0;
}

bool RecordBackedSkyrimBindings::IsEssential(ObjectRef actor_base) {
  return (AcbsFlags(records_, ToFormId(actor_base)) & 0x02) != 0;
}

papyrus::ObjectRef RecordBackedSkyrimBindings::ResolveFormRef(ObjectRef from, u32 subrecord_type) {
  if (!records_) return {};
  bethesda::GlobalFormId id = ToFormId(from);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return {};
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return {};
  const bethesda::Subrecord* sub = rec.Find(subrecord_type);
  if (!sub || sub->data.size() < 4) return {};
  u32 raw;
  std::memcpy(&raw, sub->data.data(), 4);
  bethesda::GlobalFormId resolved =
      records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
  return papyrus::ObjectRef{resolved.packed()};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetRace(ObjectRef actor_base) {
  return ResolveFormRef(actor_base, FourCc('R', 'N', 'A', 'M'));
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetHarvestIngredient(ObjectRef flora) {
  return ResolveFormRef(flora, FourCc('P', 'F', 'I', 'G'));
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetEnchantment(ObjectRef item) {
  return ResolveFormRef(item, FourCc('E', 'I', 'T', 'M'));
}

namespace {
// Bethesda game units to engine meters (the runtime's snapshot is in engine
// space). Distance is invariant under the axis swap, so a radius converts by the
// same factor.
constexpr f32 kGameUnitsToEngine = 0.01428f;
}  // namespace

void RecordBackedSkyrimBindings::UpdatePositionSnapshot(
    const std::vector<std::pair<u64, std::array<f32, 3>>>& positions) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  live_positions_.clear();
  for (const auto& [handle, pos] : positions) live_positions_[handle] = pos;
}

i32 RecordBackedSkyrimBindings::GetNearbyRefs(ObjectRef center, f32 radius) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  nearby_cache_.clear();
  auto it = live_positions_.find(center.handle);
  if (it == live_positions_.end()) return 0;
  const std::array<f32, 3> c = it->second;
  const f32 r = radius * kGameUnitsToEngine;
  const f32 r2 = r * r;
  for (const auto& [handle, pos] : live_positions_) {
    if (handle == center.handle) continue;
    const f32 dx = pos[0] - c[0], dy = pos[1] - c[1], dz = pos[2] - c[2];
    const f32 d2 = dx * dx + dy * dy + dz * dz;
    if (d2 <= r2) nearby_cache_.push_back({handle, std::sqrt(d2) / kGameUnitsToEngine});
  }
  return static_cast<i32>(nearby_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthNearbyRef(i32 index) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  if (index < 0 || index >= static_cast<i32>(nearby_cache_.size())) return {};
  return ObjectRef{nearby_cache_[static_cast<size_t>(index)].first};
}

f32 RecordBackedSkyrimBindings::GetNthNearbyDistance(i32 index) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  if (index < 0 || index >= static_cast<i32>(nearby_cache_.size())) return 0.0f;
  return nearby_cache_[static_cast<size_t>(index)].second;
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetBaseObject(ObjectRef ref) {
  return ResolveFormRef(ref, FourCc('N', 'A', 'M', 'E'));
}

bool RecordBackedSkyrimBindings::IsInterior(ObjectRef cell) {
  if (!records_) return false;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(cell), &rec)) return false;
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  return data && !data->data.empty() && (data->data[0] & 0x1);
}

f32 RecordBackedSkyrimBindings::GetCellWaterLevel(ObjectRef cell) {
  if (!records_) return 0.0f;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(cell), &rec)) return 0.0f;
  const bethesda::Subrecord* xclw = rec.Find(FourCc('X', 'C', 'L', 'W'));
  if (!xclw || xclw->data.size() < 4) return 0.0f;
  f32 value;
  std::memcpy(&value, xclw->data.data(), 4);
  return value;
}

f32 RecordBackedSkyrimBindings::GetActorValue(ObjectRef actor, const std::string& av) {
  return Av(actor, av).current;
}

f32 RecordBackedSkyrimBindings::GetBaseActorValue(ObjectRef actor, const std::string& av) {
  return Av(actor, av).base;
}

f32 RecordBackedSkyrimBindings::GetActorValuePercentage(ObjectRef actor, const std::string& av) {
  ActorValue& v = Av(actor, av);
  if (v.base <= 0.0f) return 0.0f;
  return std::clamp(v.current / v.base, 0.0f, 1.0f);
}

void RecordBackedSkyrimBindings::RaiseFormEvent(u64 target, const char* event,
                                                std::vector<papyrus::Value> args) {
  const bool dispatched = vm_ && vm_->TryCall(ObjectRef{target}, event, std::move(args));
  // REC_EVENT_TRACE logs every raised form event and whether a handler ran, so a
  // headless quest run shows which events the stage fragments actually produce.
  static const bool trace = std::getenv("REC_EVENT_TRACE") != nullptr;
  if (trace)
    REC_INFO("event {} -> 0x{:x} (handler {})", event, target, dispatched ? "ran" : "none");
}

void RecordBackedSkyrimBindings::EmitManagedEvent(host::ManagedEventId id, u64 a, u64 b, i32 i) {
  if (event_sink_) event_sink_(host::ManagedEvent{id, a, b, i, 0.0f});
}

void RecordBackedSkyrimBindings::MaybeNotifyDeath(ObjectRef actor) {
  if (GetActorValue(actor, "health") > 0.0f) {
    dead_.erase(actor.handle);  // healed or resurrected: re-arm the death event
    return;
  }
  if (!dead_.insert(actor.handle).second) return;  // already announced
  // No combat system yet, so the killer is unknown (None).
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormEvent(actor.handle, "OnDying", {none});
  RaiseFormEvent(actor.handle, "OnDeath", {none});
  EmitManagedEvent(host::ManagedEventId::kActorDied, actor.handle, 0, 0);
}

void RecordBackedSkyrimBindings::SetActorValue(ObjectRef actor, const std::string& av, f32 value) {
  ActorValue& v = Av(actor, av);
  v.base = value;
  v.current = value;
  if (Lower(av) == "health") MaybeNotifyDeath(actor);
}

void RecordBackedSkyrimBindings::ForceActorValue(ObjectRef actor, const std::string& av, f32 value) {
  SetActorValue(actor, av, value);
}

void RecordBackedSkyrimBindings::ModActorValue(ObjectRef actor, const std::string& av, f32 delta) {
  Av(actor, av).current += delta;
  if (Lower(av) == "health") MaybeNotifyDeath(actor);
}

void RecordBackedSkyrimBindings::RestoreActorValue(ObjectRef actor, const std::string& av,
                                                   f32 amount) {
  ActorValue& v = Av(actor, av);
  v.current = std::min(v.base, v.current + amount);
  if (Lower(av) == "health") MaybeNotifyDeath(actor);
}

bool RecordBackedSkyrimBindings::IsDead(ObjectRef actor) {
  return GetActorValue(actor, "health") <= 0.0f;
}

i32 RecordBackedSkyrimBindings::GetItemCount(ObjectRef container, ObjectRef item) {
  auto it = inventory_.find(container.handle);
  if (it == inventory_.end()) return 0;
  auto item_it = it->second.find(item.handle);
  return item_it == it->second.end() ? 0 : item_it->second;
}

i32 RecordBackedSkyrimBindings::GetNumItems(ObjectRef container) {
  auto it = inventory_.find(container.handle);
  return it == inventory_.end() ? 0 : static_cast<i32>(it->second.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthForm(ObjectRef container, i32 index) {
  auto it = inventory_.find(container.handle);
  if (it == inventory_.end() || index < 0) return {};
  // Every entry has a positive count (RemoveItem erases zeroed ones), so the
  // index maps directly onto the map's entries.
  for (const auto& [item, count] : it->second)
    if (index-- == 0) return ObjectRef{item};
  return {};
}

void RecordBackedSkyrimBindings::AddItem(ObjectRef container, ObjectRef item, i32 count) {
  if (count <= 0) return;
  inventory_[container.handle][item.handle] += count;
  // OnItemAdded(akBaseItem, aiItemCount, akItemReference, akSourceContainer).
  // No placed reference / source container yet, so those are None.
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormEvent(container.handle, "OnItemAdded",
                 {papyrus::Value::Object(item), papyrus::Value::Int(count), none, none});
  EmitManagedEvent(host::ManagedEventId::kItemAdded, container.handle, item.handle, count);
}

void RecordBackedSkyrimBindings::RemoveItem(ObjectRef container, ObjectRef item, i32 count) {
  if (count <= 0) return;
  auto it = inventory_.find(container.handle);
  if (it == inventory_.end()) return;
  auto item_it = it->second.find(item.handle);
  if (item_it == it->second.end()) return;
  const i32 removed = std::min(count, item_it->second);
  item_it->second = std::max(0, item_it->second - count);
  if (item_it->second == 0) it->second.erase(item_it);
  if (removed <= 0) return;
  // OnItemRemoved(akBaseItem, aiItemCount, akItemReference, akDestContainer).
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormEvent(container.handle, "OnItemRemoved",
                 {papyrus::Value::Object(item), papyrus::Value::Int(removed), none, none});
  EmitManagedEvent(host::ManagedEventId::kItemRemoved, container.handle, item.handle, removed);
}

i32 RecordBackedSkyrimBindings::GetStage(ObjectRef quest) {
  return quest_system_.GetStage(quest.handle);
}

void RecordBackedSkyrimBindings::SetStageFragment(u64 quest, i32 stage, std::string function) {
  stage_fragments_[quest][stage] = std::move(function);
}

void RecordBackedSkyrimBindings::SetSceneFragments(u64 scene, u64 owning_quest,
                                                   bethesda::SceneFragments frags) {
  scene_fragments_[scene] = SceneFragmentSet{owning_quest, std::move(frags)};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::SceneOwningQuest(papyrus::ObjectRef scene) {
  auto it = scene_fragments_.find(scene.handle);
  return papyrus::ObjectRef{it != scene_fragments_.end() ? it->second.owning_quest : 0};
}

void RecordBackedSkyrimBindings::SceneStart(papyrus::ObjectRef scene) {
  // Server-authoritative: a client mirrors quest progress via replication.
  if (replica_mode_) return;
  // Only scenes whose SCEN was parsed + SF_ script attached (by the runtime) can
  // play; an unregistered scene is a silent no-op rather than a crash.
  auto it = scene_fragments_.find(scene.handle);
  if (it == scene_fragments_.end()) return;
  std::vector<u32> phases;
  for (const auto& p : it->second.frags.phases) phases.push_back(p.phase);
  std::sort(phases.begin(), phases.end());
  phases.erase(std::unique(phases.begin(), phases.end()), phases.end());
  SceneCueSink sink;
  sink.b = this;
  scene_player_.Start(scene.handle, std::move(phases), kScenePhaseSeconds, sink);
}

void RecordBackedSkyrimBindings::SceneStop(papyrus::ObjectRef scene) {
  SceneCueSink sink;
  sink.b = this;
  scene_player_.Stop(scene.handle, sink);
}

bool RecordBackedSkyrimBindings::SceneIsPlaying(papyrus::ObjectRef scene) {
  return scene_player_.IsPlaying(scene.handle);
}

void RecordBackedSkyrimBindings::TickScenes(f32 dt) {
  if (scene_player_.playing_count() == 0) return;
  SceneCueSink sink;
  sink.b = this;
  scene_player_.Tick(dt, sink);
}

void RecordBackedSkyrimBindings::RunSceneFragment(u64 scene, u64 owning_quest,
                                                  const std::string& function) {
  // Server-authoritative: a multiplayer client mirrors quest progress via
  // replication, so it must not run scene logic itself (the fragments touch more
  // than SetStage -- ref enables, dialogue -- and only SetStage is replica-gated).
  if (replica_mode_) return;
  if (!vm_ || function.empty()) return;
  // Scene fragments call SetStage, whose stage fragment can run more fragments;
  // share the stage-fragment depth guard so a cyclic chain cannot blow the stack.
  if (fragment_depth_ >= 32) {
    REC_WARN("scene fragment recursion too deep at {}.{}", scene, function);
    return;
  }
  ++fragment_depth_;
  // Attribute world mutations during the fragment to the scene's quest, like a
  // stage fragment, so QuestWorld can roll them back. Save/restore for nesting.
  u64 prev_quest = active_quest_;
  active_quest_ = owning_quest;
  u64 before = vm_->native_call_count();
  vm_->Call(papyrus::ObjectRef{scene}, function, {});
  REC_DEBUG("scene fragment {} ran, {} native calls", function,
            vm_->native_call_count() - before);
  active_quest_ = prev_quest;
  --fragment_depth_;
}

void RecordBackedSkyrimBindings::RunSceneBegin(u64 scene) {
  ++scenes_begun_;
  auto it = scene_fragments_.find(scene);
  if (it != scene_fragments_.end())
    RunSceneFragment(scene, it->second.owning_quest, it->second.frags.begin.function);
}

void RecordBackedSkyrimBindings::RunSceneEnd(u64 scene) {
  auto it = scene_fragments_.find(scene);
  if (it != scene_fragments_.end())
    RunSceneFragment(scene, it->second.owning_quest, it->second.frags.end.function);
}

void RecordBackedSkyrimBindings::RunScenePhase(u64 scene, u32 phase, bool on_begin) {
  auto it = scene_fragments_.find(scene);
  if (it == scene_fragments_.end()) return;
  for (const auto& p : it->second.frags.phases)
    if (p.phase == phase && p.on_begin == on_begin)
      RunSceneFragment(scene, it->second.owning_quest, p.fragment.function);
}

void RecordBackedSkyrimBindings::RunStageFragment(ObjectRef quest, i32 stage) {
  if (!vm_) return;
  auto qit = stage_fragments_.find(quest.handle);
  if (qit == stage_fragments_.end()) return;
  auto fit = qit->second.find(stage);
  if (fit == qit->second.end() || fit->second.empty()) return;
  // Stage fragments call SetStage on themselves and other quests; cap the depth
  // so a cyclic chain in the data cannot blow the guest stack.
  if (fragment_depth_ >= 32) {
    REC_WARN("quest fragment recursion too deep at {}.{}", quest.handle, fit->second);
    return;
  }
  ++fragment_depth_;
  // Attribute world mutations made during this fragment to the quest, so the
  // provenance layer can roll them back. Save/restore for nested fragments.
  u64 prev_quest = active_quest_;
  active_quest_ = quest.handle;
  u64 before = vm_->native_call_count();
  vm_->Call(quest, fit->second, {});
  REC_DEBUG("quest fragment {} (stage {}) ran, {} native calls", fit->second, stage,
            vm_->native_call_count() - before);
  active_quest_ = prev_quest;
  --fragment_depth_;
}

RecordBackedSkyrimBindings::QuestRuntime& RecordBackedSkyrimBindings::Runtime(u64 quest) {
  if (auto it = quest_runtime_.find(quest); it != quest_runtime_.end()) return *it->second;
  static const std::unordered_map<i32, std::string> kNoFragments;
  auto fit = stage_fragments_.find(quest);
  const auto& fragments = fit != stage_fragments_.end() ? fit->second : kNoFragments;
  quest::QuestDef empty;
  empty.handle = quest;
  const quest::QuestDef* def = quest_system_.Definition(quest);
  quest::QuestGraph graph = quest::BuildQuestGraph(def ? *def : empty, fragments);
  auto [it, _] = quest_runtime_.emplace(quest, std::make_unique<QuestRuntime>(std::move(graph)));
  return *it->second;
}

void RecordBackedSkyrimBindings::RunScriptFragment(u64 quest, i32 node, const std::string&) {
  // RunStageFragment looks the function up itself (and keeps the recursion-depth
  // guard and logging), so the node's stored name is informational here.
  RunStageFragment(ObjectRef{quest}, node);
}

void RecordBackedSkyrimBindings::SetStage(ObjectRef quest, i32 stage) {
  if (replica_mode_) return;  // server-authoritative: stage arrives via ApplyStatus
  // The quest system owns the state and tells us whether this is a fresh stage;
  // only then do we run the stage's logic. That logic now flows through the
  // quest graph: Advance enters the stage node and dispatches its on-enter
  // actions (for imported quests, a RunScriptFragment that runs the Papyrus
  // fragment). Re-setting a done stage is a no-op, matching the game.
  if (quest_system_.SetStage(quest.handle, stage)) {
    Runtime(quest.handle).instance.Advance(stage, *this);
    EmitManagedEvent(host::ManagedEventId::kQuestStageChanged, quest.handle, 0, stage);
  }
}

bool RecordBackedSkyrimBindings::GetStageDone(ObjectRef quest, i32 stage) {
  return quest_system_.GetStageDone(quest.handle, stage);
}

std::string RecordBackedSkyrimBindings::GetJournalEntry(ObjectRef quest) {
  return quest_system_.Status(quest.handle).log_entry;
}

bool RecordBackedSkyrimBindings::IsRunning(ObjectRef quest) {
  return quest_system_.IsRunning(quest.handle);
}

void RecordBackedSkyrimBindings::StartQuest(ObjectRef quest) {
  if (replica_mode_) return;  // server starts quests; clients mirror the result
  quest_system_.StartQuest(quest.handle);
  // Kick the opening stage so the quest's logic actually begins. The start
  // stage is the lowest one carrying a fragment.
  auto qit = stage_fragments_.find(quest.handle);
  if (qit != stage_fragments_.end() && !qit->second.empty()) {
    i32 lowest = qit->second.begin()->first;
    for (const auto& [stage, fn] : qit->second) lowest = std::min(lowest, stage);
    SetStage(quest, lowest);
  }
}

void RecordBackedSkyrimBindings::StopQuest(ObjectRef quest) {
  if (replica_mode_) return;
  quest_system_.StopQuest(quest.handle);
}

void RecordBackedSkyrimBindings::ResetQuest(ObjectRef quest) {
  if (replica_mode_) return;
  quest_system_.ResetQuest(quest.handle);
  quest_runtime_.erase(quest.handle);  // rebuild a fresh traversal on next start
  // Roll back everything the quest spawned/placed/moved in the world.
  if (world_sink_) world_sink_->CleanupQuest(quest.handle);
}

bool RecordBackedSkyrimBindings::IsQuestActive(ObjectRef quest) {
  return quest_system_.IsActive(quest.handle);
}

void RecordBackedSkyrimBindings::SetQuestActive(ObjectRef quest, bool active) {
  if (replica_mode_) return;
  quest_system_.SetActive(quest.handle, active);
}

void RecordBackedSkyrimBindings::SetObjectiveDisplayed(ObjectRef quest, i32 objective,
                                                       bool displayed) {
  if (replica_mode_) return;
  quest_system_.SetObjectiveDisplayed(quest.handle, objective, displayed);
}

void RecordBackedSkyrimBindings::SetObjectiveCompleted(ObjectRef quest, i32 objective,
                                                       bool completed) {
  if (replica_mode_) return;
  quest_system_.SetObjectiveCompleted(quest.handle, objective, completed);
}

bool RecordBackedSkyrimBindings::IsObjectiveDisplayed(ObjectRef quest, i32 objective) {
  return quest_system_.IsObjectiveDisplayed(quest.handle, objective);
}

bool RecordBackedSkyrimBindings::IsObjectiveCompleted(ObjectRef quest, i32 objective) {
  return quest_system_.IsObjectiveCompleted(quest.handle, objective);
}

}  // namespace rec::script::skyrim
