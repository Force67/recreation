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

  // The next `count` bytes in place. Empty, with the cursor latched off, past
  // the end.
  ByteSpan Read(mem_size count) {
    if (!Take(count))
      return {};
    const ByteSpan out(data_.data() + pos_, count);
    pos_ += count;
    return out;
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

base::String ReadWString(Cursor& c) {
  const u16 size = c.U16();
  if (size == 0 || !c.CountFits(size, 1))
    return {};
  base::Vector<u8> text;
  text.resize(size);
  c.Bytes(text.data(), size);
  if (!c.ok())
    return {};
  return base::String(reinterpret_cast<const char*>(text.data()), size);
}

bool SkipExtraList(Cursor& c, InventoryItem* item, ReferenceChange* ref);
bool SkipInlineActorBase(Cursor& c, u32 flags);

// One entry of an extra-data list: a type byte and a payload whose size the
// type decides. A type this table does not name ends the walk, because the list
// carries no lengths and a guessed size turns every byte after it into noise
// that still looks like data.
//
// The types that are read out rather than stepped over were each pinned down by
// the same two steps: the change flag that puts them in the list, and a value
// the game authors elsewhere that they have to agree with. See the individual
// cases; the sizes of the rest are what makes the payloads consume exactly.
bool SkipExtraEntry(Cursor& c, InventoryItem* item, ReferenceChange* ref) {
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
    case 33: {  // ownership: who the thing belongs to, so who it is stolen from
      const ChangeRef owner = c.Ref();
      if (!c.ok())
        return false;
      if (item)
        item->owner = owner;
      return true;
    }
    case 36: {  // count: how many copies the list stands for
      const u16 count = c.U16();
      if (!c.ok())
        return false;
      if (item) {
        item->has_stack_count = true;
        item->stack_count = count;
      }
      return true;
    }
    case 37: {  // health, i.e. the temper multiplier
      const f32 health = c.F32();
      if (!c.ok())
        return false;
      if (item) {
        item->has_health = true;
        item->health = health;
      }
      return true;
    }
    case 40: {  // charge left in the enchantment
      const f32 charge = c.F32();
      if (!c.ok())
        return false;
      if (item) {
        item->has_charge = true;
        item->charge = charge;
      }
      return true;
    }
    case 42: {  // lock: level, flags, the key that opens it, then eight bytes
      const u8 level = c.U8();
      const u8 flags = c.U8();
      const ChangeRef key = c.Ref();
      c.Skip(4);  // zero on every lock in the reference save
      c.Skip(4);  // 0 or 1, and nothing says which state is which
      if (!c.ok())
        return false;
      if (ref) {
        ref->has_lock = true;
        ref->lock_level = level;
        ref->lock_flags = flags;
        ref->lock_key = key;
      }
      return true;
    }
    case 44: {  // map marker: one byte of flags, and nothing else
      const u8 flags = c.U8();
      if (!c.ok())
        return false;
      if (ref) {
        ref->has_map_marker = true;
        ref->map_marker_flags = flags;
      }
      return true;
    }
    case 62: {  // poison: what it is coated with and how many uses are left
      const ChangeRef poison = c.Ref();
      const u32 doses = c.U32();
      if (!c.ok())
        return false;
      if (item) {
        item->poison = poison;
        item->poison_doses = doses;
      }
      return true;
    }
    case 73: {  // hotkey: the entry is the favourite, the byte is the slot
      const u8 slot = c.U8();
      if (!c.ok())
        return false;
      if (item) {
        item->favourite = true;
        item->hotkey = slot;
      }
      return true;
    }
    case 155: {  // enchantment: the form and the charge it holds when full
      const ChangeRef enchantment = c.Ref();
      const u16 charge = c.U16();
      if (!c.ok())
        return false;
      if (item) {
        item->enchantment = enchantment;
        item->enchantment_charge = charge;
      }
      return true;
    }
    case 156: {  // soul: 1 petty through 5 grand
      const u8 soul = c.U8();
      if (!c.ok())
        return false;
      if (item)
        item->soul = soul;
      return true;
    }
    case 0: case 29: case 32: case 53: case 61:
      return c.ok();
    case 31: case 77: case 84: case 150:
      c.Skip(1);
      return c.ok();
    case 79:
      c.Skip(2);
      return c.ok();
    case 26: case 28: case 34: case 35: case 56: case 69: case 72:
    case 101: case 104: case 112: case 133: case 142: case 146: case 157:
    case 164:
      c.Skip(3);
      return c.ok();
    case 30: case 39: case 47: case 83: case 85: case 89:
    case 93: case 160:
      c.Skip(4);
      return c.ok();
    case 46:
      c.Skip(5);
      return c.ok();
    case 102: case 159:
      c.Skip(6);
      return c.ok();
    case 88: case 106: case 149:
      c.Skip(7);
      return c.ok();
    case 176:
      c.Skip(8);
      return c.ok();
    case 169:
      c.Skip(12);
      return c.ok();
    case 25:
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
      if (a.none() && b.none() && kind == -2) {
        base::String name = ReadWString(c);
        if (item)
          item->name = base::move(name);
      }
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
// the list belongs to an inventory stack; `ref` collects the ones that describe
// the reference itself.
bool SkipExtraList(Cursor& c, InventoryItem* item, ReferenceChange* ref) {
  const u32 count = c.VsVal();
  if (!c.CountFits(count, 1))
    return false;
  for (u32 i = 0; i < count; ++i) {
    if (!SkipExtraEntry(c, item, ref))
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
    item.described_copies = lists;
    for (u32 k = 0; k < lists; ++k) {
      if (!SkipExtraList(c, &item, nullptr))
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

// One perk array: a vsval count and that many entries `stride` bytes wide, each
// opening with a ref. A four byte entry carries one more byte after the ref.
bool ReadPerkArray(Cursor& c, u32 stride, u32 max_count, base::Vector<ActorPerk>& out) {
  const u32 count = c.VsVal();
  if (count == 0 || count > max_count || !c.CountFits(count, stride))
    return false;
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    ActorPerk entry;
    entry.perk = c.Ref();
    if (stride == 4)
      entry.rank = c.U8();
    if (!c.ok())
      return false;
    // A perk array names a perk in every slot: no holes, no unused kind, and a
    // rank byte inside the range a perk record can author.
    if (entry.perk.none())
      return false;
    if (stride == 4 && (entry.rank == 0 || entry.rank > kMaxActorPerkRank))
      return false;
    out.push_back(entry);
  }
  return c.ok();
}

bool SameRef(ChangeRef a, ChangeRef b) {
  return a.kind == b.kind && a.value == b.value;
}

// The pair of arrays at `at`: the ranked one, then a shorter bare one whose
// entries are all in it. Only the ranked one is kept; the second exists to be
// checked against, see FindActorPerks.
// The perk and spell scans try every byte offset of an actor block, and a
// failing attempt costs as much as a succeeding one. On a real save almost
// every offset is rejected in the first few bytes, but a crafted block can make
// thousands of them parse thousands of entries each, turning the load into
// minutes of work. `budget` is the shared ceiling on entries examined across a
// whole scan; running it out abandons the search, which yields nothing rather
// than a wrong answer.
constexpr u64 kActorScanBudget = 4ull * 1000 * 1000;

bool ReadPerkArrays(ByteSpan block, mem_size at, u64& budget, base::Vector<ActorPerk>& out) {
  Cursor c(ByteSpan(block.data() + at, block.size() - at));
  base::Vector<ActorPerk> ranked;
  if (!ReadPerkArray(c, 4, kMaxActorPerks, ranked))
    return false;
  base::Vector<ActorPerk> bare;
  if (!ReadPerkArray(c, 3, static_cast<u32>(ranked.size()), bare))
    return false;
  const u64 cost = static_cast<u64>(bare.size()) * ranked.size() + ranked.size();
  budget = cost >= budget ? 0 : budget - cost;
  if (budget == 0)
    return false;
  for (const ActorPerk& entry : bare) {
    bool listed = false;
    for (const ActorPerk& have : ranked)
      listed = listed || SameRef(have.perk, entry.perk);
    if (!listed)
      return false;
  }
  out = base::move(ranked);
  return true;
}

// The perks sit at the far end of the actor block, some 85 KB past the value
// tables and behind the same undecoded AI process state, so they are searched
// for rather than walked to. There is no sentinel to key off here, so what is
// searched for is the shape itself: a vsval count and that many (perk ref,
// rank byte) entries, immediately followed by a second vsval count and that
// many bare perk refs, every entry of the second array also appearing in the
// first.
//
// That nesting is what makes the pair identifiable rather than a run of bytes
// that reads like one. Over the reference player's 110036 byte actor block
// exactly one offset satisfies it: 297 ranked perks (DestructionMaster100,
// Necromage, DLC1VampiricBite ...) followed by 293 of the same refs again. As
// with the value tables, a block that offers a second reading yields nothing,
// so a guess is never reported as a perk.
void FindActorPerks(ByteSpan block, base::Vector<ActorPerk>& out) {
  bool ambiguous = false;
  u64 budget = kActorScanBudget;
  for (mem_size i = 0; i + 5 <= block.size() && budget != 0; ++i) {
    base::Vector<ActorPerk> perks;
    if (!ReadPerkArrays(block, i, budget, perks))
      continue;
    if (!out.empty()) {
      ambiguous = true;
      break;
    }
    out = base::move(perks);
  }
  if (ambiguous)
    out.clear();
}

// One added-spell array: a u32 count and that many refs. A spell list names a
// real form in every slot and never twice, which is what tells it from a run of
// bytes that happens to read like refs.
bool ReadSpellArray(ByteSpan block, mem_size at, base::Vector<ChangeRef>& out) {
  if (block.size() - at < 4)
    return false;
  const u32 count = ReadU32(block, at);
  if (count < kMinActorSpells || count > kMaxActorSpells)
    return false;
  at += 4;
  if ((block.size() - at) / 3 < count)
    return false;
  base::Vector<ChangeRef> found;
  found.reserve(count);
  for (u32 i = 0; i < count; ++i, at += 3) {
    ChangeRef ref;
    const u32 packed = u32(block[at]) << 16 | u32(block[at + 1]) << 8 | u32(block[at + 2]);
    ref.kind = static_cast<ChangeRefKind>((packed >> 22) & 0x3);
    ref.value = packed & 0x3FFFFF;
    if (ref.none() || ref.kind == ChangeRefKind::kUnused)
      return false;
    for (const ChangeRef& have : found) {
      if (have.kind == ref.kind && have.value == ref.value)
        return false;
    }
    found.push_back(ref);
  }
  out = base::move(found);
  return true;
}

// The spells the actor was given during play sit in the same undecoded stretch
// of the actor block the perks do, so they are searched for on the same terms:
// the shape is a u32 count followed by that many distinct, non-empty refs, and a
// block that offers more than one reading of it yields nothing at all.
//
// The length is what makes the shape identifiable, and where the cut goes was
// swept rather than picked. Over the reference save's 20658 actor blocks, a
// minimum of 12 entries leaves 65 blocks with exactly one reading and 64 of them
// read a run of references rather than spells; 24 leaves 5, all still
// references; 32 leaves one in the whole file, the player's 161, every one of
// which resolves to a SPEL record in the masters. An actor with fewer added
// spells than that yields nothing, which is the honest answer here.
void FindActorSpells(ByteSpan block, base::Vector<ChangeRef>& out) {
  bool ambiguous = false;
  u64 budget = kActorScanBudget;
  for (mem_size i = 0; i + 4 <= block.size() && budget != 0; ++i) {
    base::Vector<ChangeRef> spells;
    if (!ReadSpellArray(block, i, spells))
      continue;
    budget = spells.size() >= budget ? 0 : budget - spells.size();
    if (!out.empty()) {
      ambiguous = true;
      break;
    }
    out = base::move(spells);
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

  // Three states with no payload at all: a container the player emptied, and a
  // door or activator's open state. Read before the walk so a payload that stops
  // short still reports them; a reference carrying the empty flag writes no
  // inventory group at all (all 3433 of them in the reference save), so this
  // flag is the only record that its contents are gone.
  result.emptied = (form.flags & kRefrChangeEmpty) != 0;
  result.open = (form.flags & kRefrChangeOpenState) != 0;
  result.open_default = (form.flags & kRefrChangeOpenDefaultState) != 0;

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
  if ((form.flags & extra_flags) && !SkipExtraList(c, nullptr, &result))
    return Truncated(c, result, out);

  if (form.flags & (kRefrChangeInventory | kRefrChangeLeveledInventory)) {
    if (!DecodeInventory(c, result.inventory))
      return Truncated(c, result, out);
    result.inventory_complete = true;
  }

  // CHANGE_REFR_PROMOTED writes no group of its own: what it announces is an
  // extra-data entry of type 140 in the list above, which the reference save
  // puts on 19620 of the 19620 references that set the bit. Reading a ref array
  // for it here cost 2701 references their last group: with it the walk lands on
  // the last byte of 102196 of the save's 106098 REFR payloads, without it
  // 104897.
  if (form.flags & kRefrChangeAnimation)
    SkipSizedBytes(c);
  if (!c.ok())
    return Truncated(c, result, out);

  // Three states with no payload at all: a container the player emptied, and a
  // door or activator's open state. A reference carrying the empty flag writes
  // no inventory group (all 3433 of them), so this flag is the only record that
  // its contents are gone.
  result.emptied = (form.flags & kRefrChangeEmpty) != 0;
  result.open = (form.flags & kRefrChangeOpenState) != 0;
  result.open_default = (form.flags & kRefrChangeOpenDefaultState) != 0;

  // A plain reference is nothing but its groups, so its payload has to end
  // exactly here. When it does not, the walk went wrong somewhere it could not
  // detect, and reporting the inventory it thinks it read would be reporting
  // noise.
  if (!actor && c.remaining() != 0) {
    result.inventory.clear();
    result.inventory_complete = false;
    return Truncated(c, result, out);
  }

  if (actor) {
    FindActorValues(c.rest(), result.actor_values);
    FindActorPerks(c.rest(), result.perks);
    FindActorSpells(c.rest(), result.spells);
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

  if (form.flags & kActorBaseChangeFullName) {
    const u16 size = c.U16();
    if (!c.CountFits(size, 1))
      return false;
    const ByteSpan text = c.Read(size);
    if (!c.ok())
      return false;
    result.has_full_name = true;
    result.full_name = base::String(reinterpret_cast<const char*>(text.data()), text.size());
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

  if (form.flags & kActorBaseChangeClass)
    c.Ref();

  if (form.flags & kActorBaseChangeRace) {
    result.has_race = true;
    result.race = c.Ref();
    result.original_race = c.Ref();
    if (!c.ok())
      return false;
  }

  if (form.flags & kActorBaseChangeFace) {
    // The group leads with a byte that says whether a face follows at all. It
    // reads 1 on the one payload in the reference save that carries the group;
    // a face written as absent has not been seen.
    const bool present = c.U8() != 0;
    if (!c.ok())
      return false;
    if (present) {
      result.has_face = true;
      result.face.hair_color = c.Ref();
      result.face.skin_tone = c.U32();
      result.face.face_texture = c.Ref();
      const u32 parts = c.VsVal();
      if (!c.CountFits(parts, 3))
        return false;
      for (u32 i = 0; i < parts; ++i)
        result.face.head_parts.push_back(c.Ref());
      const bool morphs = c.U8() != 0;
      if (!c.ok())
        return false;
      if (morphs) {
        result.face.has_morphs = true;
        const u32 sliders = c.U32();
        if (!c.CountFits(sliders, 4))
          return false;
        for (u32 i = 0; i < sliders; ++i)
          result.face.morphs.push_back(c.F32());
        const u32 presets = c.U32();
        if (!c.CountFits(presets, 4))
          return false;
        for (u32 i = 0; i < presets; ++i)
          result.face.presets.push_back(c.I32());
      }
      if (!c.ok())
        return false;
    }
  }

  if (form.flags & kActorBaseChangeGender) {
    result.has_gender = true;
    result.female = c.U8() != 0;
  }
  if (form.flags & kActorBaseChangeDefaultOutfit)
    c.Ref();
  if (form.flags & kActorBaseChangeSleepOutfit)
    c.Ref();
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeWordOfPower(const ChangeForm& form, WordOfPowerChange& out) {
  if (form.type != ChangeFormType::kWoop)
    return false;
  if (!VersionSupported(form))
    return false;

  WordOfPowerChange result;
  Cursor c(PayloadOf(form));
  if (form.flags & kWordOfPowerChangeFormFlags) {
    const u32 flags = c.U32();
    c.U16();  // the same trailing short every form-flag group writes, always 0
    if (!c.ok())
      return false;
    result.known = (flags & kWordOfPowerKnown) != 0;
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

  // The groups come in this order, which is not the flag order: the exterior
  // coordinates and the detach time lead, the form flags follow them. A cell
  // that carries both reads its coordinate at byte 2, which is what pins the
  // order down (the sizes alone do not, they add up either way).
  if (form.flags & kCellChangeExteriorGrid) {
    result.has_grid = true;
    result.grid_world = c.U16();
    result.grid_x = static_cast<i8>(c.U8());
    result.grid_y = static_cast<i8>(c.U8());
  }
  if (form.flags & kCellChangeDetachTime) {
    result.has_detach_time = true;
    result.detach_time = c.U32();
  }
  if (form.flags & kCellChangeFormFlags) {
    result.has_form_flags = true;
    result.form_flags = c.U16();
  }
  if (!c.ok())
    return false;

  if (form.flags & kCellChangeVisited) {
    if (result.has_grid || !result.has_detach_time) {
      // An exterior is one map tile, so its bits stand alone. A cell still in
      // memory writes neither coordinate nor detach time and is one too: the
      // player is standing in it, so it is an exterior of the streamed world.
      CellVisitedGrid grid;
      c.Bytes(grid.bits, sizeof(grid.bits));
      result.visited.push_back(grid);
    } else {
      // An interior's local map is a counted set of tiles at their own
      // coordinates, which is how it covers a room bigger than one tile.
      const u32 count = c.VsVal();
      if (!c.CountFits(count, 34))
        return false;
      for (u32 i = 0; i < count; ++i) {
        CellVisitedGrid grid;
        grid.has_tile = true;
        grid.tile_x = static_cast<i8>(c.U8());
        grid.tile_y = static_cast<i8>(c.U8());
        c.Bytes(grid.bits, sizeof(grid.bits));
        result.visited.push_back(grid);
      }
    }
  }

  // The owner sits behind the map data, not with the groups that lead the
  // payload: reading it before them leaves the tile count three bytes off and
  // the cell comes back with no map at all.
  if (form.flags & kCellChangeOwnership)
    c.Ref();

  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeLocation(const ChangeForm& form, LocationChange& out) {
  if (form.type != ChangeFormType::kLctn)
    return false;
  if (!VersionSupported(form))
    return false;

  LocationChange result;
  Cursor c(PayloadOf(form));

  // Two groups, cleared first. The order is what the eight records carrying
  // both settle: they open with the two cleared bytes and the keyword count
  // follows, and reading them the other way round makes the count a state byte
  // and leaves the payload two bytes long.
  if (form.flags & kLocationChangeCleared) {
    result.has_cleared = true;
    result.cleared = c.U8() != 0;
    result.cleared_extra = c.U8();
  }
  if (form.flags & kLocationChangeKeywordData) {
    const u32 count = c.VsVal();
    if (!c.CountFits(count, 7))
      return false;
    result.keyword_data.reserve(count);
    for (u32 i = 0; i < count; ++i) {
      LocationKeywordValue entry;
      entry.keyword = c.Ref();
      entry.value = c.F32();
      result.keyword_data.push_back(entry);
    }
  }
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeEncounterZone(const ChangeForm& form, EncounterZoneChange& out) {
  if (form.type != ChangeFormType::kEczn)
    return false;
  if (!VersionSupported(form))
    return false;

  EncounterZoneChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kEncounterZoneChangeGameData) {
    result.has_game_data = true;
    result.stamp_a = c.U32();
    result.stamp_b = c.U32();
    result.stamp_c = c.U32();
    result.level = c.U32();
  }
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeLeveledList(const ChangeForm& form, LeveledListChange& out) {
  if (form.type != ChangeFormType::kLvli && form.type != ChangeFormType::kLvln)
    return false;
  if (!VersionSupported(form))
    return false;

  LeveledListChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kLeveledListChangeAddedObject) {
    // A bare byte, not the vsval every other count in this format uses: over
    // the reference save's 27 leveled lists a u8 count consumes all 27 payloads
    // to the last byte and a vsval count consumes none of them.
    const u32 count = c.U8();
    if (!c.CountFits(count, 7))
      return false;
    result.added.reserve(count);
    for (u32 i = 0; i < count; ++i) {
      LeveledListEntry entry;
      entry.form = c.Ref();
      entry.level = c.U16();
      entry.count = c.U16();
      result.added.push_back(entry);
    }
  }
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeBook(const ChangeForm& form, BookChange& out) {
  if (form.type != ChangeFormType::kBook)
    return false;
  if (!VersionSupported(form))
    return false;

  BookChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kBookChangeRead) {
    result.has_flags = true;
    result.flags = c.U8();
    result.read = (result.flags & kBookFlagRead) != 0;
  }
  result.skill_taken = (form.flags & kBookChangeSkillTaken) != 0;
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeIngredient(const ChangeForm& form, IngredientChange& out) {
  if (form.type != ChangeFormType::kIngr)
    return false;
  if (!VersionSupported(form))
    return false;

  IngredientChange result;
  Cursor c(PayloadOf(form));

  if (form.flags & kIngredientChangeUse) {
    result.has_known_effects = true;
    result.known_effects = c.U32();
  }
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

bool DecodeRelationship(const ChangeForm& form, RelationshipChange& out) {
  if (form.type != ChangeFormType::kRela)
    return false;
  if (!VersionSupported(form))
    return false;

  RelationshipChange result;
  Cursor c(PayloadOf(form));

  // Which shape the payload takes is decided by the id, not by a flag, the same
  // way a created reference carries its base object: a relationship no plugin
  // authors has to say who it is between, one that exists as a record does not.
  result.created = form.form_id >> 24 == 0xff;
  if (result.created) {
    result.parent = c.Ref();
    result.child = c.Ref();
    result.association = c.Ref();
  }
  if (form.flags & kRelationshipChangeData)
    result.rank = c.U32();
  if (!c.ok())
    return false;

  result.decoded_bytes = c.offset();
  out = base::move(result);
  return true;
}

}  // namespace rx::bethesda
