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

  // What the cursor has not consumed. Only the actor block is read this way,
  // because it is searched rather than walked.
  ByteSpan rest() const {
    return ok_ ? ByteSpan(data_.data() + pos_, data_.size() - pos_) : ByteSpan();
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

// Which shape the transform group at the head of a reference payload takes.
enum class InitialDataKind : u8 { kNone, kMoved, kCellChanged, kCreated };

// Change flags that put an extra-data list in the payload. The two record types
// name different kinds of extra data, so they trigger on different bits, and a
// bit in the wrong set means reading a list that is not there.
constexpr u32 kRefrExtraDataFlags = 0x00000040 |  // ownership
                                    0x00000400 |  // item data
                                    0x00000800 |  // ammo
                                    0x00001000 |  // lock
                                    0x00020000 |  // door teleport
                                    0x02000000 |  // promoted
                                    0x04000000 |  // activating children
                                    0x20000000 |  // encounter zone
                                    0x80000000;   // game-only extras
constexpr u32 kAchrExtraDataFlags = 0x00000040 |  // ownership
                                    0x00000200 |  // package data
                                    0x00000800 |  // merchant container
                                    0x00020000 |  // dismembered limbs
                                    0x00040000 |  // levelled actor
                                    0x02000000 |  // promoted
                                    0x04000000 |  // activating children
                                    0x20000000 |  // encounter zone
                                    0x40000000 |  // created-only extras
                                    0x80000000;   // game-only extras

// A vsval byte count followed by that many opaque bytes (havok state, the
// animation graph blob).
void SkipSizedBytes(Cursor& c) {
  const u32 size = c.VsVal();
  if (c.CountFits(size, 1))
    c.Skip(size);
}

void SkipWString(Cursor& c) {
  const u16 size = c.U16();
  if (c.CountFits(size, 1))
    c.Skip(size);
}

bool SkipExtraList(Cursor& c, InventoryItem* item);
bool SkipInlineActorBase(Cursor& c, u32 flags);

// One entry of an extra-data list: a type byte and a payload whose size the
// type decides. Only the two worn markers are read out; everything else is
// stepped over. A type this table does not name ends the walk, because the
// list carries no lengths and a guessed size turns every byte after it into
// noise that still looks like data.
bool SkipExtraEntry(Cursor& c, InventoryItem* item) {
  const u8 type = c.U8();
  if (!c.ok())
    return false;
  switch (type) {
    case 22:  // worn: the right hand, or the armour slot
      if (item)
        item->equipped = true;
      return true;
    case 23:  // worn left
      if (item)
        item->equipped_left = true;
      return true;
    case 0: case 29: case 32: case 53: case 61:
      return c.ok();
    case 31: case 44: case 73: case 77: case 84: case 150: case 156:
      c.Skip(1);
      return c.ok();
    case 36: case 79:
      c.Skip(2);
      return c.ok();
    case 26: case 28: case 33: case 34: case 35: case 56: case 69: case 72:
    case 101: case 104: case 112: case 133: case 142: case 146: case 157:
    case 164:
      c.Skip(3);
      return c.ok();
    case 30: case 37: case 39: case 40: case 47: case 83: case 85: case 89:
    case 93: case 160:
      c.Skip(4);
      return c.ok();
    case 46: case 155:
      c.Skip(5);
      return c.ok();
    case 102: case 159:
      c.Skip(6);
      return c.ok();
    case 62: case 88: case 106: case 149:
      c.Skip(7);
      return c.ok();
    case 176:
      c.Skip(8);
      return c.ok();
    case 169:
      c.Skip(12);
      return c.ok();
    case 25: case 42:
      c.Skip(13);
      return c.ok();
    case 24:
      c.Skip(19);
      return c.ok();
    case 43:
      c.Skip(28);
      return c.ok();
    case 161:
      c.Skip(88);
      return c.ok();
    case 27: case 68: {  // vsval-counted u32 arrays
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 4))
        return false;
      c.Skip(count * 4);
      return c.ok();
    }
    case 52: case 120: {  // vsval-counted 8 byte arrays
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 8))
        return false;
      c.Skip(count * 8);
      return c.ok();
    }
    case 111: {
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 11))
        return false;
      c.Skip(count * 11);
      return c.ok();
    }
    case 136: {  // the quest aliases this reference is filling
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 7))
        return false;
      c.Skip(count * 7);
      return c.ok();
    }
    case 140: {
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 3))
        return false;
      c.Skip(count * 3);
      return c.ok();
    }
    case 91: {  // faction changes, then one more faction and rank
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 4))
        return false;
      c.Skip(count * 4 + 4);
      return c.ok();
    }
    case 153: {  // display name: two refs and an integer, then a renamed string
      const ChangeRef a = c.Ref();
      const ChangeRef b = c.Ref();
      const i32 kind = c.I32();
      if (!c.ok())
        return false;
      // -2 with both refs empty is how a save spells "the player typed this
      // name in"; anything else names a form that supplies the text.
      if (a.none() && b.none() && kind == -2)
        SkipWString(c);
      return c.ok();
    }
    case 45: {  // a levelled actor's rolled-up NPC_ record, written inline
      c.Ref();
      c.Ref();
      const u32 flags = c.U32();
      if (!c.ok())
        return false;
      return SkipInlineActorBase(c, flags);
    }
    default:
      return false;
  }
}

// A vsval count and that many entries. `item` collects the worn markers when
// the list belongs to an inventory stack.
bool SkipExtraList(Cursor& c, InventoryItem* item) {
  const u32 count = c.VsVal();
  if (!c.CountFits(count, 1))
    return false;
  for (u32 i = 0; i < count; ++i) {
    if (!SkipExtraEntry(c, item))
      return false;
  }
  return c.ok();
}

// An inventory stack: what it is, how many, and one extra-data list per copy in
// the stack that carries any. Two enchanted swords in one stack write two
// lists, which is what makes this a list of lists rather than one list.
bool DecodeInventory(Cursor& c, base::Vector<InventoryItem>& out) {
  const u32 count = c.VsVal();
  if (!c.CountFits(count, 8))
    return false;
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    InventoryItem item;
    item.item = c.Ref();
    item.count = c.I32();
    const u32 lists = c.VsVal();
    if (!c.CountFits(lists, 1))
      return false;
    for (u32 k = 0; k < lists; ++k) {
      if (!SkipExtraList(c, &item))
        return false;
    }
    if (!c.ok())
      return false;
    out.push_back(item);
  }
  return true;
}

// The NPC_ body as a levelled actor's extra data embeds it. Same groups in the
// same order as DecodeActorBase, but nothing is kept: this only has to leave
// the cursor on the byte after the record.
bool SkipInlineActorBase(Cursor& c, u32 flags) {
  if (flags & 0x00000001)
    c.Skip(6);
  if (flags & 0x00000002)
    c.Skip(24);
  if (flags & 0x00000040) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 4))
      return false;
    c.Skip(count * 4);
  }
  if (flags & 0x00000010) {
    for (u32 list = 0; list < 3; ++list) {  // spells, levelled spells, shouts
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 3))
        return false;
      c.Skip(count * 3);
    }
  }
  if (flags & 0x00000008)
    c.Skip(20);
  if (flags & 0x00000020)
    SkipWString(c);
  if (flags & 0x00000200)
    c.Skip(52);
  if (flags & 0x00000400)
    c.Skip(3);
  if (flags & 0x02000000)
    c.Skip(6);
  if ((flags & 0x00000800) && c.U8() != 0) {
    c.Skip(3 + 4 + 3);
    const u32 parts = c.VsVal();
    if (!c.CountFits(parts, 3))
      return false;
    c.Skip(parts * 3);
    if (c.U8() != 0) {
      const u32 morphs = c.U32();
      if (!c.CountFits(morphs, 4))
        return false;
      c.Skip(morphs * 4);
      const u32 presets = c.U32();
      if (!c.CountFits(presets, 4))
        return false;
      c.Skip(presets * 4);
    }
  }
  if (flags & 0x01000000)
    c.Skip(1);
  if (flags & 0x00001000)
    c.Skip(3);
  if (flags & 0x00002000)
    c.Skip(3);
  return c.ok();
}

u32 ReadU32(ByteSpan bytes, mem_size at) {
  return u32(bytes[at]) | u32(bytes[at + 1]) << 8 | u32(bytes[at + 2]) << 16 |
         u32(bytes[at + 3]) << 24;
}

// Six tables of (value index, value, modifier slot) laid end to end, each with
// its own u32 count. Appends the first non-empty one, because which table is
// which is not established and they agree on the values that are in both.
bool ReadActorValueTables(ByteSpan block,
                          mem_size at,
                          base::Vector<ActorValueEntry>* out) {
  base::Vector<ActorValueEntry> found;
  bool kept = false;
  for (u32 table = 0; table < kActorValueTableCount; ++table) {
    if (block.size() - at < 4)
      return false;
    const u32 count = ReadU32(block, at);
    at += 4;
    if (count > kMaxActorValueEntries || (block.size() - at) / 9 < count)
      return false;
    for (u32 i = 0; i < count; ++i, at += 9) {
      ActorValueEntry entry;
      entry.index = ReadU32(block, at);
      const u32 bits = ReadU32(block, at + 4);
      std::memcpy(&entry.value, &bits, sizeof(entry.value));
      entry.modifier = block[at + 8];
      // A real table indexes an actor value and names one of the three
      // modifier slots; anything outside that is a table this is not.
      if (entry.index >= kActorValueIndexCount || entry.modifier > 2)
        return false;
      if (!kept)
        found.push_back(entry);
    }
    kept = kept || count != 0;
  }
  if (found.empty())
    return false;
  *out = base::move(found);
  return true;
}

// The actor block opens with the actor's AI process state, which is variable
// length and not decoded, so the value tables inside it cannot be walked to.
// They can still be found: the block's fixed fields end with a -1.0f sentinel
// and the tables start eight bytes past it. A block that offers more than one
// reading yields nothing at all, so a guess is never reported as a value.
void FindActorValues(ByteSpan block, base::Vector<ActorValueEntry>& out) {
  constexpr u32 kEndOfFixedFields = 0xBF800000u;  // -1.0f
  constexpr mem_size kTablesPastSentinel = 8;
  bool ambiguous = false;
  for (mem_size i = 0; i + 4 <= block.size(); ++i) {
    if (ReadU32(block, i) != kEndOfFixedFields)
      continue;
    if (block.size() - i < kTablesPastSentinel)
      break;
    base::Vector<ActorValueEntry> tables;
    if (!ReadActorValueTables(block, i + kTablesPastSentinel, &tables))
      continue;
    if (!out.empty()) {
      ambiguous = true;
      break;
    }
    out = base::move(tables);
  }
  if (ambiguous)
    out.clear();
}

// The walk stopped inside a group it could not size. Everything read before
// that still holds, and decoded_bytes says where the understanding ends.
bool Truncated(const Cursor& c, ReferenceChange& result, ReferenceChange& out) {
  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
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

  const bool actor = form.type == ChangeFormType::kAchr;
  ReferenceChange result;
  Cursor c(PayloadOf(form));

  // The transform group's shape is chosen by the flags, not written down: a
  // reference the save itself created carries its base object with it, one that
  // changed cell carries where it started as well, and one that only moved
  // carries the transform alone. Getting this wrong shifts every group after it.
  const InitialDataKind initial =
      form.form_id >> 24 == 0xff
          ? InitialDataKind::kCreated
          : (form.flags & (kRefrChangePromoted | kRefrChangeCellChanged)
                 ? InitialDataKind::kCellChanged
                 : (form.flags & (kRefrChangeMoved | kRefrChangeHavokMoved)
                        ? InitialDataKind::kMoved
                        : InitialDataKind::kNone));

  if (initial != InitialDataKind::kNone) {
    result.moved = true;
    result.parent = c.Ref();
    for (f32& v : result.position)
      v = c.F32();
    for (f32& v : result.rotation)
      v = c.F32();
    if (initial == InitialDataKind::kCreated) {
      c.Skip(1);
      result.base_object = c.Ref();
    } else if (initial == InitialDataKind::kCellChanged) {
      c.Ref();    // the cell the reference started in
      c.Skip(4);  // two shorts whose meaning is not established
    }
    if (!c.ok())
      return false;
  }

  // From here on a group that does not read is reported as a short decode
  // rather than a refusal: the transform above already stands on its own, and
  // it is what the player's own placement is read from.
  if (form.flags & kRefrChangeHavokMoved)
    SkipSizedBytes(c);

  // An actor writes eight bytes here that no flag asks for: an integer that
  // reads -1 whenever the actor is not being tracked, then four bytes whose
  // meaning is not established. A plain reference writes neither.
  if (actor)
    c.Skip(8);
  if (!c.ok())
    return Truncated(c, result, out);

  if (form.flags & kRefrChangeFormFlags) {
    const u32 flags = c.U32();
    const u16 extra = c.U16();
    if (!c.ok())
      return Truncated(c, result, out);
    result.has_form_flags = true;
    result.form_flags = flags;
    result.form_flags_extra = extra;
  }
  if (form.flags & kRefrChangeBaseObject)
    result.base_object = c.Ref();
  if (form.flags & kRefrChangeScale) {
    const f32 scale = c.F32();
    if (!c.ok())
      return Truncated(c, result, out);
    result.has_scale = true;
    result.scale = scale;
  }
  if (!c.ok())
    return Truncated(c, result, out);

  // The extra-data list is only present when one of the flags that names a kind
  // of extra data is set, and the two record types name different ones.
  const u32 extra_flags = actor ? kAchrExtraDataFlags : kRefrExtraDataFlags;
  if ((form.flags & extra_flags) && !SkipExtraList(c, nullptr))
    return Truncated(c, result, out);

  if (form.flags & (kRefrChangeInventory | kRefrChangeLeveledInventory)) {
    if (!DecodeInventory(c, result.inventory))
      return Truncated(c, result, out);
    result.inventory_complete = true;
  }

  if (!actor && (form.flags & kRefrChangePromoted)) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 3))
      return Truncated(c, result, out);
    c.Skip(count * 3);
  }
  if (form.flags & kRefrChangeAnimation)
    SkipSizedBytes(c);
  if (!c.ok())
    return Truncated(c, result, out);

  // A plain reference is nothing but its groups, so its payload has to end
  // exactly here. When it does not, the walk went wrong somewhere it could not
  // detect, and reporting the inventory it thinks it read would be reporting
  // noise.
  if (!actor && c.remaining() != 0) {
    result.inventory.clear();
    result.inventory_complete = false;
    return Truncated(c, result, out);
  }

  if (actor)
    FindActorValues(c.rest(), result.actor_values);

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

  if (form.flags & kActorBaseChangeFormFlags)
    c.Skip(6);

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

  if (form.flags & kActorBaseChangeSpells) {
    base::Vector<ChangeRef>* const lists[3] = {&result.spells, &result.levelled_spells,
                                               &result.shouts};
    for (base::Vector<ChangeRef>* list : lists) {
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 3))
        return false;
      for (u32 i = 0; i < count; ++i)
        list->push_back(c.Ref());
    }
    if (!c.ok())
      return false;
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
    result.infamy_violent = c.U32();
    result.infamy_non_violent = c.U32();
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
