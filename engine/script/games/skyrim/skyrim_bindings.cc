#include "script/games/skyrim/skyrim_bindings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "bethesda/record.h"

namespace rec::script::skyrim {
namespace {

using papyrus::ObjectRef;

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

i32 RecordBackedSkyrimBindings::GetFormType(ObjectRef form) {
  if (!records_) return 0;
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(ToFormId(form));
  return stored ? FormTypeFromSignature(stored->header.type) : 0;
}

std::string RecordBackedSkyrimBindings::GetName(ObjectRef form) {
  if (!records_) return "";
  bethesda::Record record;
  if (!records_->Parse(ToFormId(form), &record)) return "";
  const bethesda::Subrecord* full = record.Find(FourCc('F', 'U', 'L', 'L'));
  if (!full) return "";
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
  positions_[ref.handle] = {x, y, z};
}

f32 RecordBackedSkyrimBindings::GetDistance(ObjectRef a, ObjectRef b) {
  std::array<f32, 3> pa = Position(a);
  std::array<f32, 3> pb = Position(b);
  f32 dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void RecordBackedSkyrimBindings::MoveTo(ObjectRef ref, ObjectRef target) {
  positions_[ref.handle] = Position(target);
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
  auto it = faction_ranks_.find(actor.handle);
  if (it == faction_ranks_.end()) return -2;  // not in faction
  auto rank_it = it->second.find(faction.handle);
  return rank_it == it->second.end() ? -2 : rank_it->second;
}

void RecordBackedSkyrimBindings::SetFactionRank(ObjectRef actor, ObjectRef faction, i32 rank) {
  faction_ranks_[actor.handle][faction.handle] = rank;
}

bool RecordBackedSkyrimBindings::IsInFaction(ObjectRef actor, ObjectRef faction) {
  auto it = faction_ranks_.find(actor.handle);
  return it != faction_ranks_.end() && it->second.count(faction.handle);
}

void RecordBackedSkyrimBindings::AddToFaction(ObjectRef actor, ObjectRef faction) {
  faction_ranks_[actor.handle].try_emplace(faction.handle, 0);
}

void RecordBackedSkyrimBindings::RemoveFromFaction(ObjectRef actor, ObjectRef faction) {
  auto it = faction_ranks_.find(actor.handle);
  if (it != faction_ranks_.end()) it->second.erase(faction.handle);
}

i32 RecordBackedSkyrimBindings::GetReaction(ObjectRef faction, ObjectRef other) {
  auto it = reactions_.find(faction.handle);
  if (it == reactions_.end()) return 0;
  auto other_it = it->second.find(other.handle);
  return other_it == it->second.end() ? 0 : other_it->second;
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

void RecordBackedSkyrimBindings::SetActorValue(ObjectRef actor, const std::string& av, f32 value) {
  ActorValue& v = Av(actor, av);
  v.base = value;
  v.current = value;
}

void RecordBackedSkyrimBindings::ForceActorValue(ObjectRef actor, const std::string& av, f32 value) {
  SetActorValue(actor, av, value);
}

void RecordBackedSkyrimBindings::ModActorValue(ObjectRef actor, const std::string& av, f32 delta) {
  Av(actor, av).current += delta;
}

void RecordBackedSkyrimBindings::RestoreActorValue(ObjectRef actor, const std::string& av,
                                                   f32 amount) {
  ActorValue& v = Av(actor, av);
  v.current = std::min(v.base, v.current + amount);
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

void RecordBackedSkyrimBindings::AddItem(ObjectRef container, ObjectRef item, i32 count) {
  if (count <= 0) return;
  inventory_[container.handle][item.handle] += count;
}

void RecordBackedSkyrimBindings::RemoveItem(ObjectRef container, ObjectRef item, i32 count) {
  if (count <= 0) return;
  auto it = inventory_.find(container.handle);
  if (it == inventory_.end()) return;
  auto item_it = it->second.find(item.handle);
  if (item_it == it->second.end()) return;
  item_it->second = std::max(0, item_it->second - count);
  if (item_it->second == 0) it->second.erase(item_it);
}

i32 RecordBackedSkyrimBindings::GetStage(ObjectRef quest) { return quests_[quest.handle].stage; }

void RecordBackedSkyrimBindings::SetStage(ObjectRef quest, i32 stage) {
  QuestState& q = quests_[quest.handle];
  q.stage = stage;
  q.stage_done[stage] = true;
  q.running = true;  // setting a stage implies the quest is running
}

bool RecordBackedSkyrimBindings::GetStageDone(ObjectRef quest, i32 stage) {
  auto it = quests_.find(quest.handle);
  if (it == quests_.end()) return false;
  auto stage_it = it->second.stage_done.find(stage);
  return stage_it != it->second.stage_done.end() && stage_it->second;
}

bool RecordBackedSkyrimBindings::IsRunning(ObjectRef quest) { return quests_[quest.handle].running; }

void RecordBackedSkyrimBindings::StartQuest(ObjectRef quest) {
  quests_[quest.handle].running = true;
}

void RecordBackedSkyrimBindings::StopQuest(ObjectRef quest) {
  quests_[quest.handle].running = false;
}

void RecordBackedSkyrimBindings::ResetQuest(ObjectRef quest) { quests_[quest.handle] = QuestState{}; }

bool RecordBackedSkyrimBindings::IsQuestActive(ObjectRef quest) {
  return quests_[quest.handle].active;
}

void RecordBackedSkyrimBindings::SetQuestActive(ObjectRef quest, bool active) {
  quests_[quest.handle].active = active;
}

void RecordBackedSkyrimBindings::SetObjectiveDisplayed(ObjectRef quest, i32 objective,
                                                       bool displayed) {
  quests_[quest.handle].objective_displayed[objective] = displayed;
}

void RecordBackedSkyrimBindings::SetObjectiveCompleted(ObjectRef quest, i32 objective,
                                                       bool completed) {
  quests_[quest.handle].objective_completed[objective] = completed;
}

bool RecordBackedSkyrimBindings::IsObjectiveDisplayed(ObjectRef quest, i32 objective) {
  auto it = quests_.find(quest.handle);
  if (it == quests_.end()) return false;
  auto obj_it = it->second.objective_displayed.find(objective);
  return obj_it != it->second.objective_displayed.end() && obj_it->second;
}

bool RecordBackedSkyrimBindings::IsObjectiveCompleted(ObjectRef quest, i32 objective) {
  auto it = quests_.find(quest.handle);
  if (it == quests_.end()) return false;
  auto obj_it = it->second.objective_completed.find(objective);
  return obj_it != it->second.objective_completed.end() && obj_it->second;
}

}  // namespace rec::script::skyrim
