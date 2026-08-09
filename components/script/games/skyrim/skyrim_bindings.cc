#include <base/algorithm.h>
#include <base/containers/array.h>
#include <base/containers/pair.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>
#include <base/option.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>
#include <cstdio>
#include <mutex>

#include "components/script/games/skyrim/skyrim_bindings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "components/bethesda/actor_stats.h"
#include "components/bethesda/record.h"
#include "components/quest/quest_def.h"
#include "components/quest/quest_import.h"
#include "components/script/games/skyrim/skyrim_condition_context.h"
#include "components/script/papyrus/alias_handle.h"
#include "components/script/papyrus/fiber.h"
#include "components/script/papyrus/vm.h"
#include "core/log.h"

namespace rx::script::skyrim {
namespace {

using papyrus::ObjectRef;

// RX_EVENT_TRACE logs every raised form event; was a function-local static.
base::Option<bool> EventTrace{"event.trace", false, "RX_EVENT_TRACE"};

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

base::String Lower(base::String s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Record signature -> Papyrus Form.GetType() value (the standard FormType enum).
// A common subset; unmapped records report 0 (None).
i32 FormTypeFromSignature(u32 type) {
  switch (type) {
    case FourCc('K', 'Y', 'W', 'D'):
      return 4;
    case FourCc('F', 'A', 'C', 'T'):
      return 11;
    case FourCc('R', 'A', 'C', 'E'):
      return 15;
    case FourCc('M', 'G', 'E', 'F'):
      return 19;
    case FourCc('S', 'P', 'E', 'L'):
      return 23;
    case FourCc('S', 'C', 'R', 'L'):
      return 24;
    case FourCc('A', 'C', 'T', 'I'):
      return 25;
    case FourCc('A', 'R', 'M', 'O'):
      return 27;
    case FourCc('B', 'O', 'O', 'K'):
      return 28;
    case FourCc('C', 'O', 'N', 'T'):
      return 29;
    case FourCc('D', 'O', 'O', 'R'):
      return 30;
    case FourCc('I', 'N', 'G', 'R'):
      return 31;
    case FourCc('L', 'I', 'G', 'H'):
      return 32;
    case FourCc('M', 'I', 'S', 'C'):
      return 33;
    case FourCc('S', 'T', 'A', 'T'):
      return 35;
    case FourCc('F', 'L', 'O', 'R'):
      return 40;
    case FourCc('F', 'U', 'R', 'N'):
      return 41;
    case FourCc('W', 'E', 'A', 'P'):
      return 42;
    case FourCc('A', 'M', 'M', 'O'):
      return 43;
    case FourCc('N', 'P', 'C', '_'):
      return 44;
    case FourCc('K', 'E', 'Y', 'M'):
      return 46;
    case FourCc('A', 'L', 'C', 'H'):
      return 47;
    case FourCc('S', 'L', 'G', 'M'):
      return 53;
    case FourCc('W', 'T', 'H', 'R'):
      return 55;
    case FourCc('C', 'E', 'L', 'L'):
      return 61;
    case FourCc('R', 'E', 'F', 'R'):
      return 62;
    case FourCc('A', 'C', 'H', 'R'):
      return 63;
    case FourCc('Q', 'U', 'S', 'T'):
      return 78;
    default:
      return 0;
  }
}

f32 DefaultActorValue(const base::String& av) {
  base::String a = Lower(av);
  if (a == "health" || a == "magicka" || a == "stamina")
    return 100.0f;
  return 0.0f;
}

// The NPC_ ACBS flags word (bit 0 Female, bit 1 Essential, bit 5 Unique).
u32 AcbsFlags(const bethesda::RecordStore* records, bethesda::GlobalFormId id) {
  if (!records)
    return 0;
  bethesda::Record rec;
  if (!records->Parse(id, &rec))
    return 0;
  const bethesda::Subrecord* acbs = rec.Find(FourCc('A', 'C', 'B', 'S'));
  if (!acbs || acbs->data.size() < 4)
    return 0;
  u32 flags;
  std::memcpy(&flags, acbs->data.data(), 4);
  return flags;
}

}  // namespace

bethesda::GlobalFormId RecordBackedSkyrimBindings::ToFormId(ObjectRef ref) const {
  u64 handle = ref.handle;
  {
    std::lock_guard<std::mutex> lock(source_forms_mutex_);
    auto* source = source_forms_.find(handle);
    if (source != nullptr)
      handle = *source;
  }
  return bethesda::GlobalFormId{static_cast<u16>(handle >> 32), static_cast<u32>(handle)};
}

void RecordBackedSkyrimBindings::SetSourceForm(u64 handle, u64 source) {
  std::lock_guard<std::mutex> lock(source_forms_mutex_);
  source_forms_[handle] = source;
}

void RecordBackedSkyrimBindings::SetRuntimeForm(u64 owner, u64 source, u64 runtime) {
  if (owner == 0 || source == 0 || runtime == 0 || source == runtime)
    return;
  std::lock_guard<std::mutex> lock(source_forms_mutex_);
  source_forms_[runtime] = source;
  runtime_forms_[owner][source] = runtime;
}

void RecordBackedSkyrimBindings::SetDoorStateLocal(u64 handle, bool locked, bool open) {
  locks_[handle].locked = locked;
  open_[handle] = open;
}

u32 RecordBackedSkyrimBindings::GetFormId(ObjectRef form) {
  return static_cast<u32>(form.handle);
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetLinkedRef(ObjectRef ref, ObjectRef keyword) {
  if (!records_ || ref.handle == 0)
    return {};
  bethesda::GlobalFormId id = ToFormId(ref);
  bethesda::Record rec;
  if (!records_->Parse(id, &rec))
    return {};
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  const u16 plugin = stored ? stored->winning_plugin : id.plugin;
  // XLKR is an optional keyword form id (4 bytes) followed by the linked ref form
  // id (4); older entries carry only the ref. Match the keyword when one is asked.
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type != FourCc('X', 'L', 'K', 'R'))
      continue;
    u32 kw = 0, target = 0;
    if (sub.data.size() >= 8) {
      std::memcpy(&kw, sub.data.data(), 4);
      std::memcpy(&target, sub.data.data() + 4, 4);
    } else if (sub.data.size() >= 4) {
      std::memcpy(&target, sub.data.data(), 4);
    } else {
      continue;
    }
    if (keyword.handle != 0) {
      bethesda::GlobalFormId kw_id = records_->ResolveFrom(bethesda::RawFormId{kw}, plugin);
      if (kw_id.packed() != keyword.handle)
        continue;
    }
    bethesda::GlobalFormId tgt = records_->ResolveFrom(bethesda::RawFormId{target}, plugin);
    u64 handle = tgt.packed();
    if (handle != 0) {
      std::lock_guard<std::mutex> lock(source_forms_mutex_);
      auto* owner = runtime_forms_.find(ref.handle);
      if (owner != nullptr) {
        if (const u64* runtime = owner->find(handle))
          handle = *runtime;
      }
    }
    return handle ? papyrus::ObjectRef{handle} : papyrus::ObjectRef{};
  }
  return {};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetParentCell(ObjectRef ref) {
  if (!records_ || ref.handle == 0)
    return {};
  bethesda::GlobalFormId cell = records_->InteriorCellOfRef(ToFormId(ref));
  return cell.packed() ? papyrus::ObjectRef{cell.packed()} : papyrus::ObjectRef{};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetForm(u32 form_id) {
  if (!records_)
    return {};
  // Runtime form id: high byte is the load-order index, low 24 bits the local
  // id. (Light/ESL forms 0xFE... are not resolved here and yield None.)
  bethesda::GlobalFormId id{static_cast<u16>((form_id >> 24) & 0xFF), form_id & 0x00FFFFFFu};
  if (!records_->Find(id))
    return {};
  return papyrus::ObjectRef{id.packed()};
}

void RecordBackedSkyrimBindings::EraseRefAlias(u64 ref, u64 alias_handle) {
  auto* it = ref_to_aliases_.find(ref);
  if (it == nullptr)
    return;
  base::Vector<u64>& v = *it;
  base::EraseIf(v, [alias_handle](u64 h) { return h == alias_handle; });
  if (v.empty())
    ref_to_aliases_.erase(ref);
}

void RecordBackedSkyrimBindings::AliasForceRefTo(ObjectRef alias, ObjectRef ref) {
  if (replica_mode_ || !papyrus::IsAliasHandle(alias.handle))
    return;
  // Maintain the reverse ref->alias index so the filled actor's death reaches its
  // alias script; drop the previous fill's link before overwriting it.
  if (auto* it = alias_fills_.find(alias.handle); it != nullptr)
    EraseRefAlias(*it, alias.handle);
  if (ref.handle == 0) {
    alias_fills_.erase(alias.handle);
  } else {
    alias_fills_[alias.handle] = ref.handle;
    ref_to_aliases_[ref.handle].push_back(alias.handle);
  }
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetAliasLocation(ObjectRef alias) {
  const auto* it = location_fills_.find(alias.handle);
  return it == nullptr ? ObjectRef{} : ObjectRef{*it};
}

void RecordBackedSkyrimBindings::ForceAliasLocation(ObjectRef alias, ObjectRef location) {
  if (replica_mode_)
    return;
  if (location.handle == 0)
    location_fills_.erase(alias.handle);
  else
    location_fills_[alias.handle] = location.handle;
}

f32 RecordBackedSkyrimBindings::GetKeywordData(ObjectRef form, ObjectRef keyword) {
  const auto it = keyword_data_.find({form.handle, keyword.handle});
  return it == keyword_data_.end() ? 0.0f : it->second;
}

void RecordBackedSkyrimBindings::SetKeywordData(ObjectRef form, ObjectRef keyword, f32 value) {
  if (!replica_mode_)
    keyword_data_[{form.handle, keyword.handle}] = value;
}

int RecordBackedSkyrimBindings::FillFindMatchingAliases(ObjectRef quest, ObjectRef location) {
  if (!records_ || replica_mode_)
    return 0;
  const quest::QuestDef* def = quest_system_.Definition(quest.handle);
  if (!def)
    return 0;
  // The Location's LCSR is one packed array of {LocationRefType:u32, Reference:u32}
  // (stride 8). Group the placed refs (resolved to engine handles) by ref-type,
  // walking up the parent chain (LCTN PNAM): a room or dungeon location inherits
  // the refs its city or hold lists, which is where most casts are tagged.
  base::UnorderedMap<u64, base::Vector<u64>> by_type;
  constexpr u32 kLcsr = FourCc('L', 'C', 'S', 'R');
  constexpr u32 kPnam = FourCc('P', 'N', 'A', 'M');
  bethesda::GlobalFormId loc_id = ToFormId(location);
  for (int level = 0; level < 4 && loc_id.plugin != 0xffff; ++level) {
    bethesda::Record loc;
    if (!records_->Parse(loc_id, &loc))
      break;
    for (const bethesda::Subrecord& s : loc.subrecords) {
      if (s.type != kLcsr)
        continue;
      for (size_t i = 0; i + 8 <= s.data.size(); i += 8) {
        u32 rt = 0, ref = 0;
        std::memcpy(&rt, s.data.data() + i, 4);
        std::memcpy(&ref, s.data.data() + i + 4, 4);
        const bethesda::GlobalFormId rt_id =
            records_->ResolveFrom(bethesda::RawFormId{rt}, loc_id.plugin);
        const bethesda::GlobalFormId ref_id =
            records_->ResolveFrom(bethesda::RawFormId{ref}, loc_id.plugin);
        if (rt_id.plugin != 0xffff && ref_id.plugin != 0xffff)
          by_type[rt_id.packed()].push_back(ref_id.packed());
      }
    }
    const bethesda::Subrecord* parent = loc.Find(kPnam);
    if (!parent || parent->data.size() < 4)
      break;
    u32 raw = 0;
    std::memcpy(&raw, parent->data.data(), 4);
    if (raw == 0)
      break;
    loc_id = records_->ResolveFrom(bethesda::RawFormId{raw}, loc_id.plugin);
  }
  if (by_type.empty())
    return 0;

  // Alias ALRT form ids resolve against the quest record's plugin.
  const bethesda::RecordStore::StoredRecord* qstored = records_->Find(ToFormId(quest));
  const u16 qplugin = qstored ? qstored->winning_plugin : static_cast<u16>(quest.handle >> 32);

  // Assign a distinct placed ref to each find-matching alias of its ref-type.
  base::UnorderedMap<u64, size_t> cursor;
  int filled = 0;
  for (const quest::AliasDef& a : def->aliases) {
    if (!a.find_matching || a.ref_type_raw == 0)
      continue;
    const u64 rt_key = records_->ResolveFrom(bethesda::RawFormId{a.ref_type_raw}, qplugin).packed();
    auto* it = by_type.find(rt_key);
    if (it == nullptr)
      continue;
    size_t& c = cursor[rt_key];
    if (c >= it->size())
      continue;  // out of matching refs for this type
    AliasForceRefTo(ObjectRef{papyrus::EncodeAliasHandle(quest.handle, a.id)},
                    ObjectRef{(*it)[c++]});
    ++filled;
  }
  return filled;
}

void RecordBackedSkyrimBindings::AliasClear(ObjectRef alias) {
  if (replica_mode_)
    return;
  if (auto* it = alias_fills_.find(alias.handle); it != nullptr)
    EraseRefAlias(*it, alias.handle);
  alias_fills_.erase(alias.handle);
}

papyrus::ObjectRef RecordBackedSkyrimBindings::AliasReference(ObjectRef alias) {
  if (!papyrus::IsAliasHandle(alias.handle))
    return {};
  // A runtime fill (ForceRefTo) wins over the authored rule; it was a valid ref
  // when set, so it is returned without re-validating against records.
  if (const auto* it = alias_fills_.find(alias.handle); it != nullptr)
    return papyrus::ObjectRef{*it};
  if (!records_)
    return {};
  const u64 quest = papyrus::AliasHandleQuest(alias.handle);
  const i32 alias_id = static_cast<i32>(papyrus::AliasHandleAliasId(alias.handle));
  const quest::QuestDef* def = quest_system_.Definition(quest);
  if (!def)
    return {};
  const quest::AliasDef* a = def->FindAlias(alias_id);
  if (!a)
    return {};
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
  } else if (a->external_quest_raw != 0 && a->external_alias >= 0) {
    // An external alias reference (ALEQ + ALEA) is whatever that alias of that
    // other quest holds; the conversation quests share their cast this way. Two
    // quests can name each other, so the hop is bounded.
    static thread_local int hops = 0;
    if (hops >= 4)
      return {};
    const bethesda::GlobalFormId other =
        records_->ResolveFrom(bethesda::RawFormId{a->external_quest_raw}, plugin);
    ++hops;
    const ObjectRef out = AliasReference(
        ObjectRef{papyrus::EncodeAliasHandle(other.packed(), static_cast<u32>(a->external_alias))});
    --hops;
    return out;
  } else {
    return {};
  }
  if (ref.plugin == 0xffff || !records_->Find(ref))
    return {};
  return papyrus::ObjectRef{ref.packed()};
}

i32 RecordBackedSkyrimBindings::GetFormType(ObjectRef form) {
  if (!records_)
    return 0;
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(ToFormId(form));
  return stored ? FormTypeFromSignature(stored->header.type) : 0;
}

bool RecordBackedSkyrimBindings::RefIsType(ObjectRef ref, const base::String& type_name) {
  if (ref.handle == 0)
    return false;
  // Every reference is, at root, a Form and an ObjectReference.
  if (type_name == "ObjectReference" || type_name == "Form" || type_name == "ScriptObject")
    return true;
  const bool want_actor = type_name == "Actor";
  if (!want_actor && type_name != "ActorBase")
    return false;
  // A reference we already hold live actor state for (a spawned combatant, an
  // alias the quest handed health/factions) is an Actor even with no record.
  if (want_actor && (actor_values_.count(ref.handle) || faction_ranks_.count(ref.handle)))
    return true;
  if (!records_)
    return false;
  if (const bethesda::RecordStore::StoredRecord* stored = records_->Find(ToFormId(ref))) {
    if (!want_actor)
      return stored->header.type == FourCc('N', 'P', 'C', '_');  // ActorBase
    if (stored->header.type == FourCc('A', 'C', 'H', 'R'))
      return true;  // placed actor ref
  }
  // A placed REFR whose base object is an NPC is an actor as well.
  if (want_actor) {
    const ObjectRef base = GetBaseObject(ref);
    if (base.handle != 0 && base.handle != ref.handle)
      if (const bethesda::RecordStore::StoredRecord* bs = records_->Find(ToFormId(base)))
        return bs->header.type == FourCc('N', 'P', 'C', '_');
  }
  return false;
}

// Item record-data accessors (GetWeight, GetGoldValue, GetWeaponDamage,
// GetArmorRating) live in skyrim_bindings_item_data.cc.

base::String RecordBackedSkyrimBindings::GetName(ObjectRef form) {
  if (!records_)
    return "";
  bethesda::Record record;
  if (!records_->Parse(ToFormId(form), &record))
    return "";
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
    if (const base::String* s = strings_->Find(string_id))
      return base::String(s->c_str());
  }
  return record.GetString(FourCc('F', 'U', 'L', 'L'));  // non-localized inline text
}

bool RecordBackedSkyrimBindings::HasKeyword(ObjectRef form, ObjectRef keyword) {
  if (!records_)
    return false;
  bethesda::Record record;
  if (!records_->Parse(ToFormId(form), &record))
    return false;
  const bethesda::Subrecord* kwda = record.Find(FourCc('K', 'W', 'D', 'A'));
  if (!kwda)
    return false;
  u32 want = static_cast<u32>(keyword.handle);
  for (size_t offset = 0; offset + 4 <= kwda->data.size(); offset += 4) {
    u32 id;
    std::memcpy(&id, kwda->data.data() + offset, 4);
    if (id == want)
      return true;
  }
  return false;
}

base::Array<f32, 3> RecordBackedSkyrimBindings::Position(ObjectRef ref) {
  auto* it = positions_.find(ref.handle);
  if (it != nullptr)
    return *it;
  // Fall back to the authored placement in the REFR record (DATA = pos+rot).
  if (records_) {
    bethesda::Record record;
    if (records_->Parse(ToFormId(ref), &record)) {
      const bethesda::Subrecord* data = record.Find(FourCc('D', 'A', 'T', 'A'));
      if (data && data->data.size() >= 12) {
        base::Array<f32, 3> p{};
        std::memcpy(p.data(), data->data.data(), 12);
        return p;
      }
    }
  }
  return {0.0f, 0.0f, 0.0f};
}

f32 RecordBackedSkyrimBindings::GetPositionX(ObjectRef ref) {
  return Position(ref)[0];
}
f32 RecordBackedSkyrimBindings::GetPositionY(ObjectRef ref) {
  return Position(ref)[1];
}
f32 RecordBackedSkyrimBindings::GetPositionZ(ObjectRef ref) {
  return Position(ref)[2];
}

void RecordBackedSkyrimBindings::SetPosition(ObjectRef ref, f32 x, f32 y, f32 z) {
  if (replica_mode_)
    return;                            // server-authoritative: positions arrive replicated
  positions_[ref.handle] = {x, y, z};  // logical position for script reads
  if (!world_sink_)
    return;
  if (ref.handle == player_.handle)
    world_sink_->MovePlayer(active_quest_, 0, x, y, z);  // raw coords, no dest ref
  else
    world_sink_->MoveReference(active_quest_, ref.handle, x, y, z);
}

f32 RecordBackedSkyrimBindings::GetDistance(ObjectRef a, ObjectRef b) {
  base::Array<f32, 3> pa = Position(a);
  base::Array<f32, 3> pb = Position(b);
  f32 dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool RecordBackedSkyrimBindings::HasLos(ObjectRef viewer, ObjectRef target) {
  if (viewer.handle == 0 || target.handle == 0)
    return false;
  // Prefer the runtime's live position snapshot (a moving NPC), falling back to the
  // logical/record placement. Without occlusion geometry, sight is a distance gate:
  // a generous range that still rejects refs in distant cells.
  constexpr f32 kLosRange = 4096.0f;  // ~58 m in Skyrim units
  auto live = [&](ObjectRef ref) -> base::Array<f32, 3> {
    std::lock_guard<std::mutex> lock(live_positions_mutex_);
    auto* it = live_positions_.find(ref.handle);
    if (it != nullptr)
      return *it;
    return base::Array<f32, 3>{};  // sentinel; replaced below when absent
  };
  base::Array<f32, 3> pv = live(viewer);
  if (pv == base::Array<f32, 3>{})
    pv = Position(viewer);
  base::Array<f32, 3> pt = live(target);
  if (pt == base::Array<f32, 3>{})
    pt = Position(target);
  f32 dx = pv[0] - pt[0], dy = pv[1] - pt[1], dz = pv[2] - pt[2];
  return dx * dx + dy * dy + dz * dz <= kLosRange * kLosRange;
}

void RecordBackedSkyrimBindings::MoveTo(ObjectRef ref, ObjectRef target) {
  if (replica_mode_)
    return;
  base::Array<f32, 3> p = Position(target);
  positions_[ref.handle] = p;
  if (!world_sink_)
    return;
  if (ref.handle == player_.handle)
    world_sink_->MovePlayer(active_quest_, target.handle, p[0], p[1], p[2]);
  else
    world_sink_->MoveReference(active_quest_, ref.handle, p[0], p[1], p[2]);
}

void RecordBackedSkyrimBindings::SetEnabled(ObjectRef ref, bool enabled) {
  if (replica_mode_)
    return;
  disabled_[ref.handle] = !enabled;
  if (world_sink_)
    world_sink_->SetEnabled(active_quest_, ref.handle, enabled);
}

bool RecordBackedSkyrimBindings::IsDisabled(ObjectRef ref) {
  auto* it = disabled_.find(ref.handle);
  return it != nullptr && *it;
}

void RecordBackedSkyrimBindings::Delete(ObjectRef ref) {
  if (replica_mode_)
    return;
  if (world_sink_)
    world_sink_->DeleteReference(active_quest_, ref.handle);
}

papyrus::ObjectRef RecordBackedSkyrimBindings::PlaceAtMe(ObjectRef where,
                                                         ObjectRef base,
                                                         i32 /*count*/) {
  if (replica_mode_ || !world_sink_)
    return ObjectRef{0};
  // Spawn at the placer's position; the sink allocates the new ref handle so we
  // can return it to the script immediately. Mirror the position locally so the
  // script's GetPosition on the new ref reads back.
  base::Array<f32, 3> p = Position(where);
  u64 handle = world_sink_->SpawnReference(active_quest_, base.handle, p[0], p[1], p[2]);
  positions_[handle] = p;
  return ObjectRef{handle};
}

f32 RecordBackedSkyrimBindings::GetScale(ObjectRef ref) {
  auto* it = scales_.find(ref.handle);
  return it == nullptr ? 1.0f : *it;
}

void RecordBackedSkyrimBindings::SetScale(ObjectRef ref, f32 scale) {
  scales_[ref.handle] = scale;
}

bool RecordBackedSkyrimBindings::IsLocked(ObjectRef ref) {
  auto* it = locks_.find(ref.handle);
  if (it != nullptr)
    return it->locked;
  bethesda::Record record;
  return records_ && records_->Parse(ToFormId(ref), &record) &&
         record.Find(FourCc('X', 'L', 'O', 'C'));
}

void RecordBackedSkyrimBindings::SetLocked(ObjectRef ref, bool locked) {
  if (replica_mode_)
    return;
  locks_[ref.handle].locked = locked;
  if (world_sink_)
    world_sink_->SetLocked(active_quest_, ref.handle, locked);
}

i32 RecordBackedSkyrimBindings::GetLockLevel(ObjectRef ref) {
  auto* it = locks_.find(ref.handle);
  return it == nullptr ? 0 : it->level;
}

void RecordBackedSkyrimBindings::SetLockLevel(ObjectRef ref, i32 level) {
  locks_[ref.handle].level = level;
}

i32 RecordBackedSkyrimBindings::GetOpenState(ObjectRef ref) {
  auto* it = open_.find(ref.handle);
  if (it == nullptr)
    return 3;  // closed
  return *it ? 1 : 3;
}

void RecordBackedSkyrimBindings::SetOpen(ObjectRef ref, bool open) {
  if (replica_mode_)
    return;
  open_[ref.handle] = open;
  if (world_sink_)
    world_sink_->SetOpen(active_quest_, ref.handle, open);
}

i32 RecordBackedSkyrimBindings::GetFactionRank(ObjectRef actor, ObjectRef faction) {
  // A runtime override (Add/Remove/SetFactionRank) wins; otherwise fall back to
  // the membership authored in the NPC_ record. A removed faction is stored as the
  // -2 sentinel so the override can hide an authored membership.
  auto* it = faction_ranks_.find(actor.handle);
  if (it != nullptr) {
    if (const i32* rank = it->find(faction.handle))
      return *rank;
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
  auto* it = reactions_.find(faction.handle);
  if (it != nullptr) {
    if (const i32* reaction = it->find(other.handle))
      return *reaction;
  }
  return AuthoredFactionReaction(faction, other);
}

void RecordBackedSkyrimBindings::SetReaction(ObjectRef faction, ObjectRef other, i32 reaction) {
  reactions_[faction.handle][other.handle] = reaction;
}

i32 RecordBackedSkyrimBindings::GetCrimeGold(ObjectRef faction) {
  auto* it = crime_gold_.find(faction.handle);
  return it == nullptr ? 0 : *it;
}

void RecordBackedSkyrimBindings::SetCrimeGold(ObjectRef faction, i32 gold) {
  crime_gold_[faction.handle] = base::Max(0, gold);
}

void RecordBackedSkyrimBindings::ModCrimeGold(ObjectRef faction, i32 delta) {
  i32& gold = crime_gold_[faction.handle];
  gold = base::Max(0, gold + delta);
}

void RecordBackedSkyrimBindings::SetInfamy(ObjectRef faction, i32 violent, i32 non_violent) {
  infamy_[faction.handle] = Infamy{base::Max(0, violent), base::Max(0, non_violent)};
}

i32 RecordBackedSkyrimBindings::GetInfamyViolent(ObjectRef faction) {
  const Infamy* it = infamy_.find(faction.handle);
  return it == nullptr ? 0 : it->violent;
}

i32 RecordBackedSkyrimBindings::GetInfamyNonViolent(ObjectRef faction) {
  const Infamy* it = infamy_.find(faction.handle);
  return it == nullptr ? 0 : it->non_violent;
}

i32 RecordBackedSkyrimBindings::GetLevel(ObjectRef actor) {
  if (const i32* level = actor_levels_.find(actor.handle))
    return *level;
  // No override: read the level off the actor's own NPC_ record. An actor whose
  // base cannot be resolved is level 1 rather than a guess.
  if (!records_)
    return 1;
  const ObjectRef base = GetBaseObject(actor);
  bethesda::Record record;
  if (base.handle == 0 || !records_->Parse(ToFormId(base), &record))
    return 1;
  bethesda::ActorStats stats;
  if (!bethesda::ReadActorStats(record, player_level_, &stats))
    return 1;
  return static_cast<i32>(stats.level);
}

void RecordBackedSkyrimBindings::SetLevel(ObjectRef actor, i32 level) {
  actor_levels_[actor.handle] = base::Max(1, level);
  if (actor.handle == player_.handle)
    player_level_ = static_cast<u32>(base::Max(1, level));
}

void RecordBackedSkyrimBindings::SetPlayerControl(i32 category, bool enabled) {
  if (!player_controls_init_) {
    player_controls_.fill(true);
    player_controls_init_ = true;
  }
  if (category >= 0 && category < kControlCount)
    player_controls_[category] = enabled;
}

bool RecordBackedSkyrimBindings::IsPlayerControlEnabled(i32 category) {
  if (!player_controls_init_)
    return true;  // all enabled until something toggles
  return category >= 0 && category < kControlCount ? player_controls_[category] : true;
}

f32 RecordBackedSkyrimBindings::GetCurrentGameTime() {
  return clock_ ? static_cast<f32>(clock_->game_days()) : 0.0f;
}

f32 RecordBackedSkyrimBindings::GetRealHoursPassed() {
  return clock_ ? clock_->real_hours() : 0.0f;
}

f32 RecordBackedSkyrimBindings::GetGlobalValue(ObjectRef global) {
  // The time globals proxy the live clock rather than a stored/authored value.
  if (clock_ && global.handle != 0) {
    if (global.handle == game_hour_global_)
      return clock_->hour();
    if (global.handle == game_days_global_)
      return static_cast<f32>(clock_->game_days());
    if (global.handle == timescale_global_)
      return clock_->timescale();
  }
  auto* it = global_values_.find(global.handle);
  if (it != nullptr)
    return *it;
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
  // Writing a time global moves the clock (e.g. a script setting GameHour).
  if (clock_ && global.handle != 0) {
    if (global.handle == game_hour_global_)
      return clock_->set_hour(value);
    if (global.handle == game_days_global_)
      return clock_->set_game_days(value);
    if (global.handle == timescale_global_)
      return clock_->set_timescale(value);
  }
  global_values_[global.handle] = value;
}

RecordBackedSkyrimBindings::ActorValue& RecordBackedSkyrimBindings::Av(ObjectRef actor,
                                                                       const base::String& av) {
  auto& values = actor_values_[actor.handle];
  base::String key = Lower(av);
  auto* it = values.find(key);
  if (it != nullptr)
    return *it;
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
  if (!records_)
    return {};
  bethesda::GlobalFormId id = ToFormId(from);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored)
    return {};
  bethesda::Record rec;
  if (!records_->Parse(id, &rec))
    return {};
  const bethesda::Subrecord* sub = rec.Find(subrecord_type);
  if (!sub || sub->data.size() < 4)
    return {};
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

bool RecordBackedSkyrimBindings::IsActorRunning(ObjectRef actor) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  return running_actors_.count(actor.handle) != 0;
}

void RecordBackedSkyrimBindings::UpdateMovingActors(const base::Vector<u64>& running) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  running_actors_.clear();
  for (u64 handle : running)
    running_actors_.insert(handle);
}

void RecordBackedSkyrimBindings::UpdatePositionSnapshot(
    const base::Vector<base::Pair<u64, base::Array<f32, 3>>>& positions) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  live_positions_.clear();
  for (const auto& [handle, pos] : positions)
    live_positions_[handle] = pos;
}

void RecordBackedSkyrimBindings::UpdateInputSnapshot(
    const base::Array<u8, static_cast<size_t>(Key::kCount)>& held) {
  std::lock_guard<std::mutex> lock(input_mutex_);
  held_keys_ = held;
}

void RecordBackedSkyrimBindings::UpdateVehicleSnapshot(f32 speed, bool riding) {
  std::lock_guard<std::mutex> lock(cart_speeds_mutex_);
  cart_speed_ = speed;
  cart_riding_ = riding;
}

void RecordBackedSkyrimBindings::DriveCart(f32 steer, f32 throttle) {
  if (vehicle_sink_)
    vehicle_sink_->DriveCart(steer, throttle);
}

void RecordBackedSkyrimBindings::MoveCart(f32 x, f32 y, f32 z) {
  if (vehicle_sink_)
    vehicle_sink_->MoveRidden(x, y, z);
}

f32 RecordBackedSkyrimBindings::CartSpeed() {
  std::lock_guard<std::mutex> lock(cart_speeds_mutex_);
  return cart_speed_;
}

bool RecordBackedSkyrimBindings::IsRiding() {
  std::lock_guard<std::mutex> lock(cart_speeds_mutex_);
  return cart_riding_;
}

bool RecordBackedSkyrimBindings::InputHeld(i32 key) {
  if (key < 0 || key >= static_cast<i32>(Key::kCount))
    return false;
  std::lock_guard<std::mutex> lock(input_mutex_);
  return held_keys_[static_cast<size_t>(key)] != 0;
}

i32 RecordBackedSkyrimBindings::GetNearbyRefs(ObjectRef center, f32 radius) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  nearby_cache_.clear();
  auto* it = live_positions_.find(center.handle);
  if (it == nullptr)
    return 0;
  const base::Array<f32, 3> c = *it;
  const f32 r = radius * kGameUnitsToEngine;
  const f32 r2 = r * r;
  for (const auto& [handle, pos] : live_positions_) {
    if (handle == center.handle)
      continue;
    const f32 dx = pos[0] - c[0], dy = pos[1] - c[1], dz = pos[2] - c[2];
    const f32 d2 = dx * dx + dy * dy + dz * dz;
    if (d2 <= r2)
      nearby_cache_.push_back({handle, std::sqrt(d2) / kGameUnitsToEngine});
  }
  return static_cast<i32>(nearby_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthNearbyRef(i32 index) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  if (index < 0 || index >= static_cast<i32>(nearby_cache_.size()))
    return {};
  return ObjectRef{nearby_cache_[static_cast<size_t>(index)].first};
}

f32 RecordBackedSkyrimBindings::GetNthNearbyDistance(i32 index) {
  std::lock_guard<std::mutex> lock(live_positions_mutex_);
  if (index < 0 || index >= static_cast<i32>(nearby_cache_.size()))
    return 0.0f;
  return nearby_cache_[static_cast<size_t>(index)].second;
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetBaseObject(ObjectRef ref) {
  return ResolveFormRef(ref, FourCc('N', 'A', 'M', 'E'));
}

bool RecordBackedSkyrimBindings::IsInterior(ObjectRef cell) {
  if (!records_)
    return false;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(cell), &rec))
    return false;
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  return data && !data->data.empty() && (data->data[0] & 0x1);
}

f32 RecordBackedSkyrimBindings::GetCellWaterLevel(ObjectRef cell) {
  if (!records_)
    return 0.0f;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(cell), &rec))
    return 0.0f;
  const bethesda::Subrecord* xclw = rec.Find(FourCc('X', 'C', 'L', 'W'));
  if (!xclw || xclw->data.size() < 4)
    return 0.0f;
  f32 value;
  std::memcpy(&value, xclw->data.data(), 4);
  return value;
}

f32 RecordBackedSkyrimBindings::GetActorValue(ObjectRef actor, const base::String& av) {
  return Av(actor, av).current;
}

f32 RecordBackedSkyrimBindings::GetBaseActorValue(ObjectRef actor, const base::String& av) {
  return Av(actor, av).base;
}

f32 RecordBackedSkyrimBindings::GetActorValuePercentage(ObjectRef actor, const base::String& av) {
  ActorValue& v = Av(actor, av);
  if (v.base <= 0.0f)
    return 0.0f;
  return base::Clamp(v.current / v.base, 0.0f, 1.0f);
}

void RecordBackedSkyrimBindings::RaiseFormEvent(u64 target,
                                                const char* event,
                                                base::Vector<papyrus::Value> args) {
  const bool dispatched = vm_ && vm_->TryCall(ObjectRef{target}, event, base::move(args));
  // RX_EVENT_TRACE logs every raised form event and whether a handler ran, so a
  // headless quest run shows which events the stage fragments actually produce.
  const bool trace = bool(EventTrace);
  if (trace)
    RX_INFO("event {} -> 0x{:x} (handler {})", event, target, dispatched ? "ran" : "none");
}

void RecordBackedSkyrimBindings::RaiseFormAndAliasEvent(u64 target,
                                                        const char* event,
                                                        base::Vector<papyrus::Value> args) {
  RaiseFormEvent(target, event, args);
  // Also dispatch to every alias the ref fills. Copy the list: a handler may
  // refill/clear the alias and mutate ref_to_aliases_ mid-iteration.
  if (auto* it = ref_to_aliases_.find(target); it != nullptr)
    for (u64 alias_handle : base::Vector<u64>(*it))
      RaiseFormEvent(alias_handle, event, args);
}

base::Vector<papyrus::ObjectRef> RecordBackedSkyrimBindings::AliasesFilledBy(
    papyrus::ObjectRef ref) const {
  base::Vector<papyrus::ObjectRef> out;
  if (auto* it = ref_to_aliases_.find(ref.handle); it != nullptr) {
    out.reserve(it->size());
    for (u64 alias_handle : *it)
      out.push_back(papyrus::ObjectRef{alias_handle});
  }
  return out;
}

void RecordBackedSkyrimBindings::EmitManagedEvent(host::ManagedEventId id, u64 a, u64 b, i32 i) {
  if (event_sink_)
    event_sink_(host::ManagedEvent{id, a, b, i, 0.0f});
}

void RecordBackedSkyrimBindings::MaybeNotifyDeath(ObjectRef actor) {
  if (GetActorValue(actor, "health") > 0.0f) {
    dead_.erase(actor.handle);  // healed or resurrected: re-arm the death event
    return;
  }
  if (!dead_.insert(actor.handle))
    return;  // already announced
  // Attribute the kill to the swinging attacker when the death came from a melee
  // hit; None for scripted Kill()/environmental deaths.
  const papyrus::Value killer = papyrus::Value::Object(last_attacker_);
  RaiseFormEvent(actor.handle, "OnDying", {killer});
  // OnDeath reaches the actor and any alias it fills, so the alias's OnDeath
  // script runs, the Civil War reinforcement soldiers decrement their pool this way.
  RaiseFormAndAliasEvent(actor.handle, "OnDeath", {killer});
  EmitManagedEvent(host::ManagedEventId::kActorDied, actor.handle, last_attacker_.handle, 0);
  // Drop the dead actor from combat and tell the main-thread driver, so soldiers
  // stop swinging at a corpse. The corpse leaves combat silently (it is dead, no
  // OnCombatStateChanged). Everyone who was fighting it transitions out of combat
  // through StopCombat instead, so their OnCombatStateChanged(0) fires. Collect
  // the survivors first: StopCombat erases from combat_target_ and its handler may
  // mutate it, so we must not hold an iterator across the call.
  combat_target_.erase(actor.handle);
  base::Vector<u64> disengaging;
  for (const auto& [fighter, target] : combat_target_)
    if (target == actor.handle)
      disengaging.push_back(fighter);
  for (u64 fighter : disengaging)
    StopCombat(ObjectRef{fighter});
  if (world_sink_)
    world_sink_->ActorDied(active_quest_, actor.handle);
}

bool RecordBackedSkyrimBindings::IsInCombat(ObjectRef actor) {
  return combat_target_.count(actor.handle) != 0;
}

void RecordBackedSkyrimBindings::SetRelationshipRank(ObjectRef a, ObjectRef b, i32 rank) {
  relationship_ranks_[{base::Min(a.handle, b.handle), base::Max(a.handle, b.handle)}] = rank;
}

i32 RecordBackedSkyrimBindings::GetRelationshipRank(ObjectRef a, ObjectRef b) {
  const auto it =
      relationship_ranks_.find({base::Min(a.handle, b.handle), base::Max(a.handle, b.handle)});
  return it == relationship_ranks_.end() ? 0 : it->second;
}

ObjectRef RecordBackedSkyrimBindings::GetCombatTarget(ObjectRef actor) {
  auto* it = combat_target_.find(actor.handle);
  return it == nullptr ? ObjectRef{0} : ObjectRef{*it};
}

void RecordBackedSkyrimBindings::StartCombat(ObjectRef actor, ObjectRef target) {
  if (actor.handle == 0 || target.handle == 0 || actor.handle == target.handle)
    return;
  if (IsDead(actor) || IsDead(target))
    return;
  const bool was_in_combat = combat_target_.count(actor.handle) != 0;
  combat_target_[actor.handle] = target.handle;
  if (world_sink_)
    world_sink_->StartCombat(active_quest_, actor.handle, target.handle);
  // OnCombatStateChanged(akTarget, aeCombatState): 1 = in combat. Only on the
  // leading edge, so re-targeting a foe mid-combat stays quiet (matches Skyrim,
  // which fires the transition once per combat entry, not per target switch).
  if (!was_in_combat)
    RaiseFormAndAliasEvent(actor.handle, "OnCombatStateChanged",
                           {papyrus::Value::Object(target), papyrus::Value::Int(1)});
}

void RecordBackedSkyrimBindings::StopCombat(ObjectRef actor) {
  const bool was_in_combat = combat_target_.count(actor.handle) != 0;
  combat_target_.erase(actor.handle);
  if (world_sink_)
    world_sink_->StopCombat(active_quest_, actor.handle);
  // 0 = no longer in combat, with a None target. Only when actually leaving
  // combat, so a redundant StopCombat is silent.
  if (was_in_combat)
    RaiseFormAndAliasEvent(actor.handle, "OnCombatStateChanged",
                           {papyrus::Value::Object(ObjectRef{0}), papyrus::Value::Int(0)});
}

void RecordBackedSkyrimBindings::SetActorFollowing(ObjectRef actor, bool follow) {
  if (world_sink_)
    world_sink_->ActorFollow(active_quest_, actor.handle, follow);
}

void RecordBackedSkyrimBindings::ApplyMeleeHit(ObjectRef attacker, ObjectRef target, f32 damage) {
  if (target.handle == 0 || IsDead(target))
    return;
  last_attacker_ = attacker;
  // OnHit(akAggressor, akSource, akProjectile, abPowerAttack, abSneakAttack,
  // abBashAttack, abHitBlocked). A plain melee swing carries no weapon/spell
  // source or projectile form yet, and none of the attack-kind flags. Raised
  // before the damage lands so OnHit precedes any lethal OnDeath (fired from
  // inside ModActorValue) and a handler sees pre-hit health.
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  const papyrus::Value no = papyrus::Value::Bool(false);
  RaiseFormAndAliasEvent(target.handle, "OnHit",
                         {papyrus::Value::Object(attacker), none, none, no, no, no, no});
  ModActorValue(target, "health", -damage);  // may trigger MaybeNotifyDeath
  last_attacker_ = ObjectRef{0};
}

namespace {
// Resolves one quest-text token body (without the angle brackets), e.g.
// "Alias=City", "Alias.ShortName=Fort", "Global=CWPercentPoolRemainingAttacker".
// Returns false to leave an unrecognised token in place.
bool ExpandToken(base::StringRef token,
                 base::String* out,
                 const base::Function<base::String(base::StringRef)>& alias_name,
                 const base::Function<bool(base::StringRef, f32*)>& global_value) {
  const size_t eq = token.find('=');
  if (eq == base::StringRef::npos)
    return false;
  base::StringRef key = token.substr(0, eq);
  base::StringRef name = token.substr(eq + 1);
  const size_t dot = key.find('.');  // strip a ".ShortName"/".GetName" qualifier
  if (dot != base::StringRef::npos)
    key = key.substr(0, dot);
  base::String lower(key);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "alias") {
    *out = alias_name(name);
    return true;
  }
  if (lower == "global") {
    f32 v = 0;
    if (!global_value(name, &v))
      return false;
    char buf[32];
    if (v == std::floor(v))
      std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v));
    else
      std::snprintf(buf, sizeof(buf), "%.1f", v);
    *out = buf;
    return true;
  }
  return false;
}
}  // namespace

base::String RecordBackedSkyrimBindings::ResolveQuestText(u64 quest, const base::String& raw) {
  if (raw.find('<') == base::String::npos)
    return raw;
  const quest::QuestDef* def = quest_system_.Definition(quest);

  // <Alias=Name> -> the display name of the reference filling that named alias,
  // falling back to the alias name itself when it is unfilled (better than a raw
  // token). Names match case-insensitively.
  auto alias_name = [&](base::StringRef name) -> base::String {
    base::String want(name);
    std::transform(want.begin(), want.end(), want.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (def) {
      for (const quest::AliasDef& a : def->aliases) {
        base::String an = a.name;
        std::transform(an.begin(), an.end(), an.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (an != want)
          continue;
        const ObjectRef ref = AliasReference(ObjectRef{papyrus::EncodeAliasHandle(quest, a.id)});
        base::String n = ref.handle ? GetName(ref) : "";
        return n.empty() ? a.name : n;
      }
    }
    return base::String(name);
  };
  // <Global=Name> -> the live value of the GLOB with that editor id.
  auto global_value = [&](base::StringRef name, f32* v) -> bool {
    if (!records_)
      return false;
    const bethesda::GlobalFormId g = records_->FindGlobal(base::String(name));
    if (g.plugin == 0xffff)
      return false;
    *v = GetGlobalValue(ObjectRef{g.packed()});
    return true;
  };

  base::String out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] != '<') {
      out += raw[i++];
      continue;
    }
    const size_t end = raw.find('>', i);
    base::String repl;
    if (end != base::String::npos && ExpandToken(base::StringRef(raw).substr(i + 1, end - i - 1),
                                                 &repl, alias_name, global_value)) {
      out += repl;
      i = end + 1;
    } else {
      out += raw[i++];
    }
  }
  return out;
}

void RecordBackedSkyrimBindings::SetActorValue(ObjectRef actor, const base::String& av, f32 value) {
  ActorValue& v = Av(actor, av);
  v.base = value;
  v.current = value;
  if (Lower(av) == "health")
    MaybeNotifyDeath(actor);
}

void RecordBackedSkyrimBindings::ForceActorValue(ObjectRef actor,
                                                 const base::String& av,
                                                 f32 value) {
  SetActorValue(actor, av, value);
}

void RecordBackedSkyrimBindings::ModActorValue(ObjectRef actor, const base::String& av, f32 delta) {
  Av(actor, av).current += delta;
  if (Lower(av) == "health")
    MaybeNotifyDeath(actor);
}

void RecordBackedSkyrimBindings::RestoreActorValue(ObjectRef actor,
                                                   const base::String& av,
                                                   f32 amount) {
  ActorValue& v = Av(actor, av);
  v.current = base::Min(v.base, v.current + amount);
  if (Lower(av) == "health")
    MaybeNotifyDeath(actor);
}

bool RecordBackedSkyrimBindings::IsDead(ObjectRef actor) {
  return GetActorValue(actor, "health") <= 0.0f;
}

void RecordBackedSkyrimBindings::Resurrect(ObjectRef actor) {
  // Only bring back something that actually died: Reset() is also called on
  // living/clutter refs, and reviving those (handing them health) would be wrong.
  if (!dead_.contains(actor.handle))
    return;
  RestoreActorValue(actor, "health", 1.0e9f);  // back to full (clamped to base)
  dead_.erase(actor.handle);
  if (world_sink_)
    world_sink_->ActorResurrected(active_quest_, actor.handle);
}

void RecordBackedSkyrimBindings::SeedAuthoredInventory(ObjectRef container) {
  if (records_ == nullptr || inventory_.contains(container.handle))
    return;
  const bethesda::GlobalFormId refr = ToFormId(container);
  const bethesda::RecordStore::StoredRecord* placed = records_->Find(refr);
  if (placed == nullptr)
    return;
  bethesda::Record record;
  if (!records_->Parse(refr, &record))
    return;
  const bethesda::Subrecord* name = record.Find(FourCc('N', 'A', 'M', 'E'));
  if (name == nullptr || name->data.size() < 4)
    return;
  u32 base_raw = 0;
  std::memcpy(&base_raw, name->data.data(), 4);
  const bethesda::GlobalFormId base =
      records_->ResolveFrom(bethesda::RawFormId{base_raw}, placed->winning_plugin);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(base);
  bethesda::Record contents;
  if (stored == nullptr || !records_->Parse(base, &contents))
    return;
  // CNTO is { form id, count }. A levelled list in there is left alone: what it
  // rolls is decided at runtime and the record does not say.
  auto& bucket = inventory_[container.handle];
  for (const bethesda::Subrecord& sub : contents.subrecords) {
    if (sub.type != FourCc('C', 'N', 'T', 'O') || sub.data.size() < 8)
      continue;
    u32 item_raw = 0;
    i32 count = 0;
    std::memcpy(&item_raw, sub.data.data(), 4);
    std::memcpy(&count, sub.data.data() + 4, 4);
    if (count <= 0)
      continue;
    const bethesda::GlobalFormId item =
        records_->ResolveFrom(bethesda::RawFormId{item_raw}, stored->winning_plugin);
    bucket[item.packed()] += count;
  }
}

void RecordBackedSkyrimBindings::SetItemCount(ObjectRef container, ObjectRef item, i32 count) {
  if (container.handle == 0 || item.handle == 0)
    return;
  if (count <= 0) {
    auto* it = inventory_.find(container.handle);
    if (it != nullptr)
      it->erase(item.handle);
    return;
  }
  inventory_[container.handle][item.handle] = count;
}

i32 RecordBackedSkyrimBindings::GetItemCount(ObjectRef container, ObjectRef item) {
  auto* it = inventory_.find(container.handle);
  if (it == nullptr)
    return 0;
  const i32* item_it = it->find(item.handle);
  return item_it == nullptr ? 0 : *item_it;
}

i32 RecordBackedSkyrimBindings::GetNumItems(ObjectRef container) {
  auto* it = inventory_.find(container.handle);
  return it == nullptr ? 0 : static_cast<i32>(it->size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthForm(ObjectRef container, i32 index) {
  auto* it = inventory_.find(container.handle);
  if (it == nullptr || index < 0)
    return {};
  // Every entry has a positive count (RemoveItem erases zeroed ones), so the
  // index maps directly onto the map's entries.
  for (const auto& [item, count] : *it)
    if (index-- == 0)
      return ObjectRef{item};
  return {};
}

void RecordBackedSkyrimBindings::AddItem(ObjectRef container, ObjectRef item, i32 count) {
  if (count <= 0)
    return;
  inventory_[container.handle][item.handle] += count;
  // OnItemAdded(akBaseItem, aiItemCount, akItemReference, akSourceContainer).
  // No placed reference / source container yet, so those are None.
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormEvent(container.handle, "OnItemAdded",
                 {papyrus::Value::Object(item), papyrus::Value::Int(count), none, none});
  EmitManagedEvent(host::ManagedEventId::kItemAdded, container.handle, item.handle, count);
}

void RecordBackedSkyrimBindings::RemoveItem(ObjectRef container, ObjectRef item, i32 count) {
  if (count <= 0)
    return;
  auto* it = inventory_.find(container.handle);
  if (it == nullptr)
    return;
  i32* item_it = it->find(item.handle);
  if (item_it == nullptr)
    return;
  const i32 removed = base::Min(count, *item_it);
  *item_it = base::Max(0, *item_it - count);
  if (*item_it == 0)
    it->erase(item.handle);
  if (removed <= 0)
    return;
  // OnItemRemoved(akBaseItem, aiItemCount, akItemReference, akDestContainer).
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormEvent(container.handle, "OnItemRemoved",
                 {papyrus::Value::Object(item), papyrus::Value::Int(removed), none, none});
  EmitManagedEvent(host::ManagedEventId::kItemRemoved, container.handle, item.handle, removed);
}

void RecordBackedSkyrimBindings::EquipItem(ObjectRef actor, ObjectRef item) {
  if (actor.handle == 0 || item.handle == 0)
    return;
  if (!equipped_[actor.handle].insert(item.handle))
    return;  // already equipped
  // OnObjectEquipped(akBaseObject, akReference) on the actor and any alias it
  // fills; akReference is None since we equip base forms, not placed refs. The
  // item's own script hears OnEquipped(akActor).
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormAndAliasEvent(actor.handle, "OnObjectEquipped", {papyrus::Value::Object(item), none});
  RaiseFormEvent(item.handle, "OnEquipped", {papyrus::Value::Object(actor)});
}

void RecordBackedSkyrimBindings::UnequipItem(ObjectRef actor, ObjectRef item) {
  if (actor.handle == 0 || item.handle == 0)
    return;
  auto* it = equipped_.find(actor.handle);
  if (it == nullptr || !it->erase(item.handle))
    return;  // was not equipped
  if (it->empty())
    equipped_.erase(actor.handle);
  const papyrus::Value none = papyrus::Value::Object(ObjectRef{0});
  RaiseFormAndAliasEvent(actor.handle, "OnObjectUnequipped", {papyrus::Value::Object(item), none});
  RaiseFormEvent(item.handle, "OnUnequipped", {papyrus::Value::Object(actor)});
}

bool RecordBackedSkyrimBindings::IsEquipped(ObjectRef actor, ObjectRef item) {
  auto* it = equipped_.find(actor.handle);
  return it != nullptr && it->count(item.handle) != 0;
}

void RecordBackedSkyrimBindings::EquippedForms(ObjectRef actor,
                                               base::Vector<bethesda::GlobalFormId>& out) const {
  const auto* it = equipped_.find(actor.handle);
  if (it == nullptr)
    return;
  out.reserve(out.size() + it->size());
  for (u64 item : *it)
    out.push_back(ToFormId(ObjectRef{item}));
}

void RecordBackedSkyrimBindings::AddPerk(ObjectRef actor, ObjectRef perk, i32 rank) {
  if (actor.handle == 0 || perk.handle == 0)
    return;
  perks_[actor.handle][perk.handle] = rank < 1 ? 1 : rank;
}

void RecordBackedSkyrimBindings::RemovePerk(ObjectRef actor, ObjectRef perk) {
  auto* it = perks_.find(actor.handle);
  if (it == nullptr)
    return;
  it->erase(perk.handle);
  if (it->empty())
    perks_.erase(actor.handle);
}

bool RecordBackedSkyrimBindings::HasPerk(ObjectRef actor, ObjectRef perk) {
  auto* it = perks_.find(actor.handle);
  return it != nullptr && it->find(perk.handle) != nullptr;
}

i32 RecordBackedSkyrimBindings::GetPerkCount(ObjectRef actor) {
  auto* it = perks_.find(actor.handle);
  return it == nullptr ? 0 : static_cast<i32>(it->size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetEquippedWeapon(ObjectRef actor) {
  if (!records_)
    return {};
  auto* it = equipped_.find(actor.handle);
  if (it == nullptr)
    return {};
  for (u64 item : *it) {
    const bethesda::RecordStore::StoredRecord* stored = records_->Find(ToFormId(ObjectRef{item}));
    if (stored && stored->header.type == FourCc('W', 'E', 'A', 'P'))
      return ObjectRef{item};
  }
  return {};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetEquippedShield(ObjectRef actor) {
  if (!records_)
    return {};
  auto* it = equipped_.find(actor.handle);
  if (it == nullptr)
    return {};
  for (u64 item : *it) {
    const bethesda::GlobalFormId id = ToFormId(ObjectRef{item});
    const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
    if (!stored || stored->header.type != FourCc('A', 'R', 'M', 'O'))
      continue;
    bethesda::Record rec;
    if (!records_->Parse(id, &rec))
      continue;
    // BOD2 (or the older BODT) opens with a u32 of first-person biped-slot flags;
    // the shield slot (39) is bit 39-30 = 9. An ARMO carrying it is the shield.
    const bethesda::Subrecord* bod = rec.Find(FourCc('B', 'O', 'D', '2'));
    if (!bod)
      bod = rec.Find(FourCc('B', 'O', 'D', 'T'));
    if (!bod || bod->data.size() < 4)
      continue;
    u32 flags;
    std::memcpy(&flags, bod->data.data(), 4);
    if (flags & (1u << 9))
      return ObjectRef{item};
  }
  return {};
}

i32 RecordBackedSkyrimBindings::GetStage(ObjectRef quest) {
  return quest_system_.GetStage(quest.handle);
}

void RecordBackedSkyrimBindings::SetStageFragment(u64 quest,
                                                  i32 stage,
                                                  i32 entry,
                                                  base::String function,
                                                  quest::ConditionList conditions) {
  auto& entries = stage_fragments_[quest][stage];
  for (StageFragment& existing : entries) {
    if (existing.entry != entry)
      continue;
    existing.function = base::move(function);
    existing.conditions = base::move(conditions);
    return;
  }
  entries.push_back({entry, base::move(function), base::move(conditions)});
}

void RecordBackedSkyrimBindings::SetSceneFragments(u64 scene,
                                                   u64 owning_quest,
                                                   bethesda::SceneFragments frags) {
  scene_fragments_[scene] = SceneFragmentSet{owning_quest, base::move(frags)};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::SceneOwningQuest(papyrus::ObjectRef scene) {
  auto* it = scene_fragments_.find(scene.handle);
  return papyrus::ObjectRef{it != nullptr ? it->owning_quest : 0};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::InfoOwningQuest(papyrus::ObjectRef info) {
  auto* it = info_owning_quest_.find(info.handle);
  return papyrus::ObjectRef{it != nullptr ? *it : 0};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::PackageOwningQuest(papyrus::ObjectRef package) {
  auto* it = package_owning_quest_.find(package.handle);
  return papyrus::ObjectRef{it != nullptr ? *it : 0};
}

void RecordBackedSkyrimBindings::RunPackageFragment(u64 package, const base::String& function) {
  if (replica_mode_ || !vm_ || function.empty())
    return;
  if (fragment_depth_ >= 32) {
    RX_WARN("package fragment recursion too deep at {}.{}", package, function);
    return;
  }
  ++fragment_depth_;
  const u64 prev_quest = active_quest_;
  auto* owner = package_owning_quest_.find(package);
  active_quest_ = owner != nullptr ? *owner : 0;
  vm_->Call(papyrus::ObjectRef{package}, function, {});
  active_quest_ = prev_quest;
  --fragment_depth_;
}

void RecordBackedSkyrimBindings::DrainSceneRequests(base::Vector<SceneRequest>& out) {
  std::lock_guard<std::mutex> lock(scene_requests_mutex_);
  out.insert(out.end(), scene_requests_.begin(), scene_requests_.end());
  scene_requests_.clear();
}

void RecordBackedSkyrimBindings::SetScenePlayingLive(u64 scene, bool playing) {
  std::lock_guard<std::mutex> lock(scene_requests_mutex_);
  if (playing)
    scenes_playing_live_.insert(scene);
  else
    scenes_playing_live_.erase(scene);
}

void RecordBackedSkyrimBindings::SceneStart(papyrus::ObjectRef scene) {
  // Server-authoritative: a client mirrors quest progress via replication.
  if (replica_mode_)
    return;
  // With the runtime playing scenes in the world, hand the call over: the director
  // needs the main thread to move actors, speak the lines and frame the camera.
  if (live_scene_playback_) {
    std::lock_guard<std::mutex> lock(scene_requests_mutex_);
    scene_requests_.push_back({scene.handle, true});
    return;
  }
  // Only scenes whose SCEN was parsed + SF_ script attached (by the runtime) can
  // play; an unregistered scene is a silent no-op rather than a crash.
  auto* it = scene_fragments_.find(scene.handle);
  if (it == nullptr)
    return;
  base::Vector<u32> phases;
  for (const auto& p : it->frags.phases)
    phases.push_back(p.phase);
  base::Sort(phases.begin(), phases.end());
  phases.erase(std::unique(phases.begin(), phases.end()), phases.end());
  SceneCueSink sink;
  sink.b = this;
  scene_player_.Start(scene.handle, base::move(phases), kScenePhaseSeconds, sink);
}

void RecordBackedSkyrimBindings::SceneStop(papyrus::ObjectRef scene) {
  if (live_scene_playback_) {
    std::lock_guard<std::mutex> lock(scene_requests_mutex_);
    scene_requests_.push_back({scene.handle, false});
    return;
  }
  SceneCueSink sink;
  sink.b = this;
  scene_player_.Stop(scene.handle, sink);
}

bool RecordBackedSkyrimBindings::SceneIsPlaying(papyrus::ObjectRef scene) {
  if (live_scene_playback_) {
    std::lock_guard<std::mutex> lock(scene_requests_mutex_);
    return scenes_playing_live_.count(scene.handle) != 0;
  }
  return scene_player_.IsPlaying(scene.handle);
}

void RecordBackedSkyrimBindings::TickScenes(f32 dt) {
  if (scene_player_.playing_count() == 0)
    return;
  SceneCueSink sink;
  sink.b = this;
  scene_player_.Tick(dt, sink);
}

void RecordBackedSkyrimBindings::RunSceneFragment(u64 scene,
                                                  u64 owning_quest,
                                                  const base::String& function) {
  // Server-authoritative: a multiplayer client mirrors quest progress via
  // replication, so it must not run scene logic itself (the fragments touch more
  // than SetStage, ref enables, dialogue, and only SetStage is replica-gated).
  if (replica_mode_)
    return;
  if (!vm_ || function.empty())
    return;
  // Scene fragments call SetStage, whose stage fragment can run more fragments;
  // share the stage-fragment depth guard so a cyclic chain cannot blow the stack.
  if (fragment_depth_ >= 32) {
    RX_WARN("scene fragment recursion too deep at {}.{}", scene, function);
    return;
  }
  ++fragment_depth_;
  // Attribute world mutations during the fragment to the scene's quest, like a
  // stage fragment, so QuestWorld can roll them back. Save/restore for nesting.
  u64 prev_quest = active_quest_;
  active_quest_ = owning_quest;
  u64 before = vm_->native_call_count();
  vm_->Call(papyrus::ObjectRef{scene}, function, {});
  RX_INFO("scene: 0x{:x} fires {}, {} native call(s)", scene, function,
          vm_->native_call_count() - before);
  active_quest_ = prev_quest;
  --fragment_depth_;
}

void RecordBackedSkyrimBindings::RunSceneBegin(u64 scene) {
  ++scenes_begun_;
  auto* it = scene_fragments_.find(scene);
  if (it != nullptr)
    RunSceneFragment(scene, it->owning_quest, it->frags.begin.function);
}

void RecordBackedSkyrimBindings::RunSceneEnd(u64 scene) {
  auto* it = scene_fragments_.find(scene);
  if (it != nullptr)
    RunSceneFragment(scene, it->owning_quest, it->frags.end.function);
}

void RecordBackedSkyrimBindings::RunScenePhase(u64 scene, u32 phase, bool on_begin) {
  auto* it = scene_fragments_.find(scene);
  if (it == nullptr)
    return;
  for (const auto& p : it->frags.phases)
    if (p.phase == phase && p.on_begin == on_begin)
      RunSceneFragment(scene, it->owning_quest, p.fragment.function);
}

void RecordBackedSkyrimBindings::RunStageFragment(ObjectRef quest, i32 stage) {
  // A fragment reached from a running activation rides that activation's fiber, so
  // run it inline; the outermost engine-triggered fragment starts its own fiber so
  // a Wait inside suspends the whole thing (provenance and recursion guard with it).
  if (fiber_runner_ && !papyrus::Fiber::current()) {
    fiber_runner_([this, quest, stage] { RunStageFragmentBody(quest, stage); });
  } else {
    RunStageFragmentBody(quest, stage);
  }
}

void RecordBackedSkyrimBindings::RunStageFragmentBody(ObjectRef quest, i32 stage) {
  if (!vm_)
    return;
  auto* qit = stage_fragments_.find(quest.handle);
  if (qit == nullptr)
    return;
  auto* fit = qit->find(stage);
  if (fit == nullptr || fit->empty())
    return;
  // A stage's log entries are conditioned; the game runs the first whose gate
  // passes. Falling back to the last entry keeps stages whose conditions we
  // cannot yet evaluate behaving as they did before conditions were read.
  const StageFragment* chosen = nullptr;
  SkyrimConditionContext conditions(this);
  for (const StageFragment& candidate : *fit) {
    if (!quest::Evaluate(candidate.conditions, conditions))
      continue;
    chosen = &candidate;
    break;
  }
  if (!chosen)
    chosen = &fit->back();
  const base::String& function = chosen->function;
  if (function.empty())
    return;
  // Stage fragments call SetStage on themselves and other quests; cap the depth
  // so a cyclic chain in the data cannot blow the guest stack.
  if (fragment_depth_ >= 32) {
    RX_WARN("quest fragment recursion too deep at {}.{}", quest.handle, function);
    return;
  }
  ++fragment_depth_;
  // Attribute world mutations made during this fragment to the quest, so the
  // provenance layer can roll them back. Save/restore for nested fragments.
  u64 prev_quest = active_quest_;
  active_quest_ = quest.handle;
  u64 before = vm_->native_call_count();
  vm_->Call(quest, function, {});
  RX_DEBUG("quest fragment {} (stage {}) ran, {} native calls", function, stage,
           vm_->native_call_count() - before);
  active_quest_ = prev_quest;
  --fragment_depth_;
}

RecordBackedSkyrimBindings::QuestRuntime& RecordBackedSkyrimBindings::Runtime(u64 quest) {
  if (auto* it = quest_runtime_.find(quest); it != nullptr)
    return **it;
  // The graph only needs to know which stages carry a fragment at all, so
  // collapse each stage's conditioned entries to its first function name.
  base::UnorderedMap<i32, base::String> fragments;
  if (auto* fit = stage_fragments_.find(quest); fit != nullptr)
    for (const auto& [stage, entries] : *fit)
      if (!entries.empty())
        fragments.emplace(stage, entries.front().function);
  quest::QuestDef empty;
  empty.handle = quest;
  const quest::QuestDef* def = quest_system_.Definition(quest);
  quest::QuestGraph graph = quest::BuildQuestGraph(def ? *def : empty, fragments);
  auto [it, _] = quest_runtime_.emplace(quest, base::MakeUnique<QuestRuntime>(base::move(graph)));
  return **it;
}

void RecordBackedSkyrimBindings::RunScriptFragment(u64 quest, i32 node, const base::String&) {
  // RunStageFragment looks the function up itself (and keeps the recursion-depth
  // guard and logging), so the node's stored name is informational here.
  RunStageFragment(ObjectRef{quest}, node);
}

void RecordBackedSkyrimBindings::ApplyReplicatedStatus(const quest::QuestStatus& status) {
  // The host owns quest progression; a client mirrors its journal here. Detect a
  // genuinely new stage before applying (ApplyStatus marks it done) so a periodic
  // re-send of the same journal does not re-fire. A fresh advance emits the managed
  // event the local SetStage path would, wiring replicated questing into the C#
  // gameplay (XP rewards and the rest) the same as single-player.
  const bool fresh = !quest_system_.GetStageDone(status.handle, status.stage);
  quest_system_.ApplyStatus(status);
  if (fresh)
    EmitManagedEvent(host::ManagedEventId::kQuestStageChanged, status.handle, 0, status.stage);
}

void RecordBackedSkyrimBindings::SetHudGauge(const base::String& id,
                                             f32 fraction,
                                             const base::String& label,
                                             u32 color) {
  if (id.empty())
    return;
  const f32 clamped = base::Clamp(fraction, 0.0f, 1.0f);
  std::lock_guard<std::mutex> lock(hud_gauges_mutex_);
  for (HudGauge& g : hud_gauges_) {
    if (g.id == id) {  // update in place, preserving HUD order
      g.fraction = clamped;
      g.label = label;
      g.color = color;
      return;
    }
  }
  hud_gauges_.push_back({id, label, clamped, color});
}

void RecordBackedSkyrimBindings::ClearHudGauge(const base::String& id) {
  std::lock_guard<std::mutex> lock(hud_gauges_mutex_);
  hud_gauges_.erase(std::remove_if(hud_gauges_.begin(), hud_gauges_.end(),
                                   [&](const HudGauge& g) { return g.id == id; }),
                    hud_gauges_.end());
}

void RecordBackedSkyrimBindings::SnapshotHudGauges(base::Vector<HudGauge>& out) const {
  std::lock_guard<std::mutex> lock(hud_gauges_mutex_);
  out = hud_gauges_;
}

void RecordBackedSkyrimBindings::SetWarHold(i32 index, const base::String& name, i32 owner) {
  if (index < 0 || index > 64)
    return;  // sanity bound on the hold count
  std::lock_guard<std::mutex> lock(war_map_mutex_);
  if (static_cast<size_t>(index) >= war_holds_.size())
    war_holds_.resize(index + 1);
  war_holds_[index] = {name, owner};
}

void RecordBackedSkyrimBindings::SetWarProgress(f32 imperial_fraction) {
  std::lock_guard<std::mutex> lock(war_map_mutex_);
  war_progress_ = base::Clamp(imperial_fraction, 0.0f, 1.0f);
}

void RecordBackedSkyrimBindings::SnapshotWarMap(base::Vector<WarHold>& out,
                                                f32& imperial_fraction) const {
  std::lock_guard<std::mutex> lock(war_map_mutex_);
  out = war_holds_;
  imperial_fraction = war_progress_;
}

void RecordBackedSkyrimBindings::SetStage(ObjectRef quest, i32 stage) {
  if (replica_mode_)
    return;  // server-authoritative: stage arrives via ApplyStatus
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

base::String RecordBackedSkyrimBindings::GetJournalEntry(ObjectRef quest) {
  return quest_system_.Status(quest.handle).log_entry;
}

bool RecordBackedSkyrimBindings::IsRunning(ObjectRef quest) {
  return quest_system_.IsRunning(quest.handle);
}

void RecordBackedSkyrimBindings::StartQuest(ObjectRef quest) {
  if (replica_mode_)
    return;  // server starts quests; clients mirror the result
  // Papyrus Quest.Start() does nothing to a quest that is already running, and
  // that matters here: a restored save resumes hundreds of quests mid-story, so
  // re-kicking the opening stage would replay their start logic (which, for the
  // handful that open by moving the player, warps them off their save point).
  if (quest_system_.IsRunning(quest.handle))
    return;
  quest_system_.StartQuest(quest.handle);
  // Kick the opening stage so the quest's logic actually begins. The start
  // stage is the lowest one carrying a fragment.
  auto* qit = stage_fragments_.find(quest.handle);
  if (qit != nullptr && !qit->empty()) {
    i32 lowest = qit->begin().key();
    for (auto entry : *qit)
      lowest = base::Min(lowest, entry.key);
    SetStage(quest, lowest);
  }
}

void RecordBackedSkyrimBindings::StopQuest(ObjectRef quest) {
  if (replica_mode_)
    return;
  quest_system_.StopQuest(quest.handle);
}

void RecordBackedSkyrimBindings::ResetQuest(ObjectRef quest) {
  if (replica_mode_)
    return;
  quest_system_.ResetQuest(quest.handle);
  quest_runtime_.erase(quest.handle);  // rebuild a fresh traversal on next start
  // Roll back everything the quest spawned/placed/moved in the world.
  if (world_sink_)
    world_sink_->CleanupQuest(quest.handle);
}

bool RecordBackedSkyrimBindings::IsQuestActive(ObjectRef quest) {
  return quest_system_.IsActive(quest.handle);
}

void RecordBackedSkyrimBindings::SetQuestActive(ObjectRef quest, bool active) {
  if (replica_mode_)
    return;
  quest_system_.SetActive(quest.handle, active);
}

void RecordBackedSkyrimBindings::SetObjectiveDisplayed(ObjectRef quest,
                                                       i32 objective,
                                                       bool displayed) {
  if (replica_mode_)
    return;
  quest_system_.SetObjectiveDisplayed(quest.handle, objective, displayed);
}

void RecordBackedSkyrimBindings::SetObjectiveCompleted(ObjectRef quest,
                                                       i32 objective,
                                                       bool completed) {
  if (replica_mode_)
    return;
  quest_system_.SetObjectiveCompleted(quest.handle, objective, completed);
}

bool RecordBackedSkyrimBindings::IsObjectiveDisplayed(ObjectRef quest, i32 objective) {
  return quest_system_.IsObjectiveDisplayed(quest.handle, objective);
}

bool RecordBackedSkyrimBindings::IsObjectiveCompleted(ObjectRef quest, i32 objective) {
  return quest_system_.IsObjectiveCompleted(quest.handle, objective);
}

}  // namespace rx::script::skyrim
