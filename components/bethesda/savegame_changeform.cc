#include "components/bethesda/savegame_changeform.h"

#include <base/memory/move.h>

#include <cstring>

namespace rx::bethesda {
namespace {

// Change form versions this layer has been validated against. Anything else is
// refused rather than parsed on the assumption that the layouts held, because a
// wrong guess here silently produces plausible-looking nonsense.
constexpr u16 kMinChangeFormVersion = 74;
constexpr u16 kMaxChangeFormVersion = 78;

// Every read goes through this. Payload bytes come from a user's save file, so
// an out of range read is expected input, not a bug: the cursor latches a
// failure and keeps returning zeroes so callers can finish their walk and check
// ok() once at the end.
class Cursor {
 public:
  explicit Cursor(ByteSpan data) : data_(data) {}

  bool ok() const { return ok_; }
  mem_size offset() const { return pos_; }
  mem_size remaining() const { return ok_ ? data_.size() - pos_ : 0; }

  u8 U8() {
    if (!Take(1))
      return 0;
    return data_[pos_++];
  }

  u16 U16() {
    if (!Take(2))
      return 0;
    const u16 v = u16(data_[pos_]) | u16(data_[pos_ + 1]) << 8;
    pos_ += 2;
    return v;
  }

  u32 U32() {
    if (!Take(4))
      return 0;
    const u32 v = u32(data_[pos_]) | u32(data_[pos_ + 1]) << 8 |
                  u32(data_[pos_ + 2]) << 16 | u32(data_[pos_ + 3]) << 24;
    pos_ += 4;
    return v;
  }

  i32 I32() { return static_cast<i32>(U32()); }
  i8 I8() { return static_cast<i8>(U8()); }
  i16 I16() { return static_cast<i16>(U16()); }

  f32 F32() {
    const u32 bits = U32();
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

  // 1 to 3 bytes little endian, the low two bits holding the byte count minus
  // one and the value living in the remaining bits. The one and two byte forms
  // are the only ones the validation save exercises; the three byte form
  // follows the same rule, and a count of 3 is not a valid encoding.
  u32 VsVal() {
    const u8 first = U8();
    switch (first & 0x3) {
      case 0:
        return first >> 2;
      case 1:
        return (u32(first) | u32(U8()) << 8) >> 2;
      case 2: {
        const u32 mid = U8();
        const u32 high = U8();
        return (u32(first) | mid << 8 | high << 16) >> 2;
      }
      default:
        ok_ = false;
        return 0;
    }
  }

  // Three bytes big endian: the top two bits are the kind, the rest the value.
  ChangeRef Ref() {
    if (!Take(3))
      return {};
    const u32 packed = u32(data_[pos_]) << 16 | u32(data_[pos_ + 1]) << 8 |
                       u32(data_[pos_ + 2]);
    pos_ += 3;
    ChangeRef ref;
    ref.kind = static_cast<ChangeRefKind>((packed >> 22) & 0x3);
    ref.value = packed & 0x3FFFFF;
    return ref;
  }

  void Bytes(u8* dst, mem_size count) {
    if (!Take(count)) {
      std::memset(dst, 0, count);
      return;
    }
    std::memcpy(dst, data_.data() + pos_, count);
    pos_ += count;
  }

  void Skip(mem_size count) {
    if (Take(count))
      pos_ += count;
  }

  // Guards a count read out of the payload before it is used as a loop bound:
  // a corrupt count must not make the decoder allocate or spin.
  bool CountFits(u32 count, mem_size element_size) {
    if (!ok_)
      return false;
    if (element_size != 0 && count > remaining() / element_size) {
      ok_ = false;
      return false;
    }
    return true;
  }

 private:
  bool Take(mem_size count) {
    if (!ok_ || data_.size() - pos_ < count) {
      ok_ = false;
      return false;
    }
    return true;
  }

  ByteSpan data_;
  mem_size pos_ = 0;
  bool ok_ = true;
};

ByteSpan PayloadOf(const ChangeForm& form) {
  return ByteSpan(form.data.data(), form.data.size());
}

bool VersionSupported(const ChangeForm& form) {
  return form.version >= kMinChangeFormVersion &&
         form.version <= kMaxChangeFormVersion;
}

}  // namespace

u32 ResolveChangeRef(ChangeRef ref, base::Span<const u32> form_ids) {
  switch (ref.kind) {
    case ChangeRefKind::kFormIdIndex:
      // One-based on disk; index 0 is how a payload spells "no reference".
      if (ref.value == 0 || ref.value > form_ids.size())
        return 0;
      return form_ids[ref.value - 1];
    case ChangeRefKind::kDefaultFile:
      return ref.value;
    case ChangeRefKind::kCreated:
      return 0xFF000000u | ref.value;
    case ChangeRefKind::kUnused:
      return 0;
  }
  return 0;
}

bool DecodeReference(const ChangeForm& form, ReferenceChange& out) {
  if (form.type != ChangeFormType::kRefr && form.type != ChangeFormType::kAchr)
    return false;
  if (!VersionSupported(form))
    return false;

  ReferenceChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & (kRefrChangeMoved | kRefrChangeCellChanged)) {
    result.moved = true;
    result.parent = c.Ref();
    for (f32& v : result.position)
      v = c.F32();
    for (f32& v : result.rotation)
      v = c.F32();
    // Four bytes that always follow the transform. Their meaning is unknown and
    // they are not exposed; skipping them is what keeps the flags group below
    // aligned.
    c.Skip(4);
    if (!c.ok())
      return false;
  }

  // The havok group is variable length and undecoded, so nothing after it can
  // be located. An ACHR stops here too: it carries actor state the plain
  // reference layout does not describe, and walking on lands mid-record.
  if ((form.flags & kRefrChangeHavokMoved) ||
      form.type == ChangeFormType::kAchr) {
    result.decoded_bytes = c.offset();
    out = base::move(result);
    return true;
  }

  if (form.flags & kRefrChangeFormFlags) {
    result.has_form_flags = true;
    result.form_flags = c.U32();
    result.form_flags_extra = c.U16();
    if (!c.ok())
      return false;
  }

  // Everything between the flags and the inventory (scale, base object, the
  // extra-data blocks) is either undecoded or of unproven order, so the
  // inventory is only reachable when none of it is present.
  constexpr u32 kWalkable = kRefrChangeFormFlags | kRefrChangeMoved |
                            kRefrChangeCellChanged | kRefrChangeInventory |
                            kRefrChangeLeveledInventory;
  const bool inventory_reachable = (form.flags & ~kWalkable) == 0;

  if (inventory_reachable &&
      (form.flags & (kRefrChangeInventory | kRefrChangeLeveledInventory))) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 8))
      return false;
    result.inventory_complete = true;
    for (u32 i = 0; i < count; ++i) {
      InventoryItem item;
      item.item = c.Ref();
      item.count = c.I32();
      item.extra_count = c.VsVal();
      if (!c.ok())
        return false;
      result.inventory.push_back(item);
      if (item.extra_count != 0) {
        result.inventory_complete = false;
        break;
      }
    }
  }

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeQuest(const ChangeForm& form, QuestChange& out) {
  if (form.type != ChangeFormType::kQust)
    return false;
  if (!VersionSupported(form))
    return false;

  QuestChange result;
  Cursor c(PayloadOf(form));

  result.quest_flags = c.U8();
  result.priority = c.U8();
  if (!c.ok())
    return false;

  if (form.flags & kQuestChangeStages) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 3))
      return false;
    for (u32 i = 0; i < count; ++i) {
      QuestStageState stage;
      stage.stage = c.U16();
      stage.flags = c.U8();
      result.stages.push_back(stage);
    }
    if (!c.ok())
      return false;
  }

  if (form.flags & kQuestChangeObjectives) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 8))
      return false;
    for (u32 i = 0; i < count; ++i) {
      QuestObjectiveState objective;
      objective.index = c.U32();
      objective.state = c.U32();
      result.objectives.push_back(objective);
    }
    if (!c.ok())
      return false;
  }

  if (form.flags & kQuestChangeRunData) {
    c.Skip(1);
    const u32 count = c.U32();
    if (!c.CountFits(count, 8))
      return false;
    for (u32 i = 0; i < count; ++i) {
      QuestAliasFill fill;
      fill.alias_id = c.U32();
      c.Skip(1);
      fill.ref = c.Ref();
      result.alias_fills.push_back(fill);
    }
    if (!c.ok())
      return false;
    // What follows in this group is a second ref array and, optionally, the
    // story-manager event that started the quest. The event's parameter
    // encoding varies with the event type and is not decoded, so the walk ends
    // with the alias fills.
  }

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeActorBase(const ChangeForm& form, ActorBaseChange& out) {
  if (form.type != ChangeFormType::kNpc)
    return false;
  if (!VersionSupported(form))
    return false;

  ActorBaseChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kActorBaseChangeStats) {
    result.has_stats = true;
    result.base_flags = c.U32();
    result.magicka_offset = c.U16();
    result.stamina_offset = c.U16();
    result.level = c.U16();
    result.calc_min_level = c.U16();
    result.calc_max_level = c.U16();
    result.speed_multiplier = c.U16();
    result.disposition_base = c.U16();
    result.template_flags = c.U16();
    result.health_offset = c.I16();
    result.bleedout_override = c.U16();
    if (!c.ok())
      return false;
  }

  if (form.flags & kActorBaseChangeFactions) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 4))
      return false;
    for (u32 i = 0; i < count; ++i) {
      ActorFactionRank entry;
      entry.faction = c.Ref();
      entry.rank = c.I8();
      result.factions.push_back(entry);
    }
    if (!c.ok())
      return false;
  }

  // Undecoded and variable, and it sits ahead of the AI and skill groups.
  if (form.flags & kActorBaseChangeUnknown10) {
    result.decoded_bytes = c.offset();
    out = base::move(result);
    return true;
  }

  if (form.flags & kActorBaseChangeAi) {
    result.has_ai = true;
    result.aggression = c.U8();
    result.confidence = c.U8();
    result.energy = c.U8();
    result.morality = c.U8();
    result.mood = c.U8();
    result.assistance = c.U8();
    c.Skip(14);  // aggro radius behaviour plus the three warn/attack timers
    if (!c.ok())
      return false;
  }

  if (form.flags & kActorBaseChangeSkills) {
    result.has_skills = true;
    c.Bytes(result.skills, kActorSkillCount);
    c.Bytes(result.skill_offsets, kActorSkillCount);
    result.health = c.U16();
    result.magicka = c.U16();
    result.stamina = c.U16();
    c.Skip(2);
    result.far_away_model_distance = c.F32();
    c.Skip(4);  // geared up weapons plus three unused bytes
    if (!c.ok())
      return false;
  }

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeFaction(const ChangeForm& form, FactionChange& out) {
  if (form.type != ChangeFormType::kFact)
    return false;
  if (!VersionSupported(form))
    return false;

  FactionChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kFactionChangeReactions) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 11))
      return false;
    for (u32 i = 0; i < count; ++i) {
      FactionReaction reaction;
      reaction.faction = c.Ref();
      reaction.modifier = c.I32();
      reaction.combat_reaction = c.U32();
      result.reactions.push_back(reaction);
    }
    if (!c.ok())
      return false;
  }

  if (form.flags & kFactionChangeFormFlags) {
    result.has_form_flags = true;
    result.form_flags = c.U32();
  }

  if (form.flags & kFactionChangeCrime) {
    result.has_crime = true;
    result.crime_count_a = c.U32();
    result.crime_count_b = c.U32();
    result.crime_time_a = c.F32();
    result.crime_time_b = c.F32();
  }

  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeDialogueInfo(const ChangeForm& form, DialogueInfoChange& out) {
  if (form.type != ChangeFormType::kInfo)
    return false;
  if (!VersionSupported(form))
    return false;
  // The whole record is its flags; a payload here means the layout changed.
  if (!form.data.empty())
    return false;

  out.said = (form.flags & kInfoChangeSaid) != 0;
  return true;
}

bool DecodeCell(const ChangeForm& form, CellChange& out) {
  if (form.type != ChangeFormType::kCell)
    return false;
  if (!VersionSupported(form))
    return false;

  CellChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kCellChangeFormFlags) {
    result.has_form_flags = true;
    result.form_flags = c.U16();
  }

  result.detached = (form.flags & kCellChangeDetached) != 0;
  if (result.detached)
    c.Skip(3);

  u32 grid_count = 0;
  if (form.flags & kCellChangeVisitedGrid) {
    c.Skip(4);
    grid_count = c.VsVal();
    if (!c.CountFits(grid_count, 34))
      return false;
    for (u32 i = 0; i < grid_count; ++i) {
      CellVisitedGrid grid;
      grid.mask = c.U16();
      c.Bytes(grid.bits, sizeof(grid.bits));
      result.visited.push_back(grid);
    }
  }

  // Without the grid form the visited bits are written bare, with no mask.
  if ((form.flags & kCellChangeVisited) && grid_count == 0) {
    CellVisitedGrid grid;
    c.Bytes(grid.bits, sizeof(grid.bits));
    result.visited.push_back(grid);
  }

  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

}  // namespace rx::bethesda
