// Form record-data accessors for RecordBackedSkyrimBindings: item weight/value,
// weapon/armor stats, what a book teaches, the magic effects an ingredient/potion/
// enchantment/spell carries, a magic effect's actor value, and a spell's cost.
// All parsed from the form's record. Split out of skyrim_bindings.cc to keep that
// file from becoming a god file; the rest of the class lives there and in the
// other skyrim_bindings_*.cc units.
#include "script/games/skyrim/skyrim_bindings.h"

#include <cstring>

#include "bethesda/record.h"

namespace rx::script::skyrim {
namespace {

using papyrus::ObjectRef;

// Item records whose DATA begins with { uint32 value; float weight; }. This
// covers the bulk of inventory: weapons, armor, clutter, ingredients, soul gems
// and keys all share the layout.
bool IsValueWeightItem(u32 signature) {
  switch (signature) {
    case FourCc('W', 'E', 'A', 'P'):
    case FourCc('A', 'R', 'M', 'O'):
    case FourCc('M', 'I', 'S', 'C'):
    case FourCc('I', 'N', 'G', 'R'):
    case FourCc('S', 'L', 'G', 'M'):
    case FourCc('K', 'E', 'Y', 'M'):
      return true;
    default:
      return false;
  }
}

f32 ReadF32(const bethesda::Subrecord* sub, size_t offset) {
  if (!sub || sub->data.size() < offset + 4) return 0.0f;
  f32 value;
  std::memcpy(&value, sub->data.data() + offset, 4);
  return value;
}

u32 ReadU32(const bethesda::Subrecord* sub, size_t offset) {
  if (!sub || sub->data.size() < offset + 4) return 0;
  u32 value;
  std::memcpy(&value, sub->data.data() + offset, 4);
  return value;
}

// The actor-value name for a skill index, matching the Skyrim ActorValue enum
// (the skills run 6..23). Empty for a non-skill index. The names are the ones the
// actor-value store keys on, so a skill book's gain routes to the right value.
const char* SkillAvName(u32 index) {
  switch (index) {
    case 6: return "OneHanded";
    case 7: return "TwoHanded";
    case 8: return "Marksman";
    case 9: return "Block";
    case 10: return "Smithing";
    case 11: return "HeavyArmor";
    case 12: return "LightArmor";
    case 13: return "Pickpocket";
    case 14: return "Lockpicking";
    case 15: return "Sneak";
    case 16: return "Alchemy";
    case 17: return "Speechcraft";
    case 18: return "Alteration";
    case 19: return "Conjuration";
    case 20: return "Destruction";
    case 21: return "Illusion";
    case 22: return "Restoration";
    case 23: return "Enchanting";
    default: return "";
  }
}

// The actor-value name a magic effect modifies (MGEF primary AV index), covering
// the values the actor-value store models: the skills, the three pools and their
// regen rates, movement and carry weight, melee/unarmed damage, the resistances,
// and the school power mods. Indices match the Skyrim ActorValue enum, verified
// against the MGEF records. Empty for anything else, so the managed effect logic
// skips what it cannot apply.
const char* EffectAvName(i32 index) {
  switch (index) {
    case 24: return "Health";
    case 25: return "Magicka";
    case 26: return "Stamina";
    case 30: return "SpeedMult";
    case 32: return "CarryWeight";
    case 34: return "MeleeDamage";
    case 35: return "UnarmedDamage";
    case 39: return "DamageResist";  // armor rating
    case 40: return "PoisonResist";
    case 41: return "ResistFire";
    case 42: return "ResistShock";
    case 43: return "ResistFrost";
    case 44: return "ResistMagic";
    case 45: return "ResistDisease";
    case 108: return "AlterationPowerMod";
    case 109: return "ConjurationPowerMod";
    case 110: return "DestructionPowerMod";
    case 111: return "IllusionPowerMod";
    case 112: return "RestorationPowerMod";
    case 155: return "HealRate";
    case 156: return "MagickaRate";
    case 157: return "StaminaRate";
    default: return SkillAvName(static_cast<u32>(index));  // skills run 6..23
  }
}

}  // namespace

f32 RecordBackedSkyrimBindings::GetWeight(ObjectRef form) {
  if (!records_) return 0.0f;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(form), &rec)) return 0.0f;
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (IsValueWeightItem(rec.header.type)) return ReadF32(data, 4);
  // Potions/food/poisons (ALCH) keep weight alone in DATA; value lives in ENIT.
  if (rec.header.type == FourCc('A', 'L', 'C', 'H')) return ReadF32(data, 0);
  return 0.0f;
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetBookSpell(ObjectRef book) {
  if (!records_) return {};
  const bethesda::GlobalFormId id = ToFormId(book);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return {};
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return {};
  if (rec.header.type != FourCc('B', 'O', 'O', 'K')) return {};
  // BOOK DATA: { uint8 flags; uint8 type; uint16 pad; uint32 teaches; ... }. The
  // 0x04 flag marks a spell tome, where `teaches` is the taught spell's form id.
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 8 || (data->data[0] & 0x04) == 0) return {};
  u32 raw;
  std::memcpy(&raw, data->data.data() + 4, 4);
  return papyrus::ObjectRef{
      records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed()};
}

std::string RecordBackedSkyrimBindings::GetBookSkill(ObjectRef book) {
  if (!records_) return "";
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(book), &rec)) return "";
  if (rec.header.type != FourCc('B', 'O', 'O', 'K')) return "";
  // The 0x01 flag marks a skill book, where `teaches` is the skill's AV index.
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 8 || (data->data[0] & 0x01) == 0) return "";
  u32 index;
  std::memcpy(&index, data->data.data() + 4, 4);
  return SkillAvName(index);
}

namespace {
// Reads a one-byte soul-gem subrecord (SOUL or SLCP) off a SLGM record, 0 if the
// form is not a soul gem or the subrecord is absent.
i32 SoulGemByte(const bethesda::RecordStore* records, bethesda::GlobalFormId id, u32 subrecord) {
  if (!records) return 0;
  bethesda::Record rec;
  if (!records->Parse(id, &rec) || rec.header.type != FourCc('S', 'L', 'G', 'M')) return 0;
  const bethesda::Subrecord* sub = rec.Find(subrecord);
  return sub && !sub->data.empty() ? sub->data[0] : 0;
}
}  // namespace

i32 RecordBackedSkyrimBindings::GetSoulGemSoul(ObjectRef gem) {
  return SoulGemByte(records_, ToFormId(gem), FourCc('S', 'O', 'U', 'L'));
}

i32 RecordBackedSkyrimBindings::GetSoulGemCapacity(ObjectRef gem) {
  return SoulGemByte(records_, ToFormId(gem), FourCc('S', 'L', 'C', 'P'));
}

i32 RecordBackedSkyrimBindings::GetGoldValue(ObjectRef form) {
  if (!records_) return 0;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(form), &rec)) return 0;
  if (IsValueWeightItem(rec.header.type))
    return static_cast<i32>(ReadU32(rec.Find(FourCc('D', 'A', 'T', 'A')), 0));
  if (rec.header.type == FourCc('A', 'L', 'C', 'H'))
    return static_cast<i32>(ReadU32(rec.Find(FourCc('E', 'N', 'I', 'T')), 0));
  return 0;
}

i32 RecordBackedSkyrimBindings::GetWeaponDamage(ObjectRef weapon) {
  if (!records_) return 0;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(weapon), &rec)) return 0;
  if (rec.header.type != FourCc('W', 'E', 'A', 'P')) return 0;
  // WEAP DATA = { uint32 value; float weight; uint16 damage; }.
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 10) return 0;
  u16 damage;
  std::memcpy(&damage, data->data.data() + 8, 2);
  return damage;
}

f32 RecordBackedSkyrimBindings::GetArmorRating(ObjectRef armor) {
  if (!records_) return 0.0f;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(armor), &rec)) return 0.0f;
  if (rec.header.type != FourCc('A', 'R', 'M', 'O')) return 0.0f;
  // ARMO DNAM holds the base armor rating scaled by 100.
  const bethesda::Subrecord* dnam = rec.Find(FourCc('D', 'N', 'A', 'M'));
  if (!dnam || dnam->data.size() < 2) return 0.0f;
  u16 scaled;
  std::memcpy(&scaled, dnam->data.data(), 2);
  return scaled / 100.0f;
}

i32 RecordBackedSkyrimBindings::GetMagicEffectCount(ObjectRef item) {
  magic_effect_cache_.clear();
  if (!records_) return 0;
  const bethesda::GlobalFormId id = ToFormId(item);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return 0;
  // Ingredients, potions (ALCH), enchantments (ENCH) and spells (SPEL) all list
  // their effects the same way, so one accessor serves alchemy (matching shared
  // effects), consumables and casting (applying them) and enchanting (inspecting).
  const u32 type = rec.header.type;
  if (type != FourCc('I', 'N', 'G', 'R') && type != FourCc('A', 'L', 'C', 'H') &&
      type != FourCc('E', 'N', 'C', 'H') && type != FourCc('S', 'P', 'E', 'L'))
    return 0;

  // The effects are ordered EFID (effect form id) / EFIT ({ float magnitude;
  // uint32 area; uint32 duration }) pairs. Each EFID's id is resolved against the
  // item's load order so managed code gets a real global handle.
  MagicEffectData pending;
  bool have_effect = false;
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type == FourCc('E', 'F', 'I', 'D') && sub.data.size() >= 4) {
      u32 raw;
      std::memcpy(&raw, sub.data.data(), 4);
      pending = MagicEffectData{};
      pending.effect =
          records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed();
      have_effect = true;
    } else if (sub.type == FourCc('E', 'F', 'I', 'T') && have_effect && sub.data.size() >= 12) {
      std::memcpy(&pending.magnitude, sub.data.data(), 4);
      u32 duration;
      std::memcpy(&duration, sub.data.data() + 8, 4);
      pending.duration = static_cast<i32>(duration);
      magic_effect_cache_.push_back(pending);
      have_effect = false;
    }
  }
  return static_cast<i32>(magic_effect_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthMagicEffectId(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= magic_effect_cache_.size()) return {};
  return papyrus::ObjectRef{magic_effect_cache_[static_cast<size_t>(index)].effect};
}

f32 RecordBackedSkyrimBindings::GetNthMagicEffectMagnitude(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= magic_effect_cache_.size()) return 0.0f;
  return magic_effect_cache_[static_cast<size_t>(index)].magnitude;
}

i32 RecordBackedSkyrimBindings::GetShoutWordCount(ObjectRef shout) {
  shout_word_cache_.clear();
  if (!records_) return 0;
  const bethesda::GlobalFormId id = ToFormId(shout);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return 0;
  if (rec.header.type != FourCc('S', 'H', 'O', 'U')) return 0;

  // A SHOU lists up to three words as SNAM subrecords, each a 12-byte struct
  // { formid word (WORD/WOOP); formid spell (SPEL); float recoveryTime }. The two
  // form ids resolve against the shout's load order so managed code gets real
  // global handles.
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type != FourCc('S', 'N', 'A', 'M') || sub.data.size() < 12) continue;
    u32 word_raw, spell_raw;
    std::memcpy(&word_raw, sub.data.data(), 4);
    std::memcpy(&spell_raw, sub.data.data() + 4, 4);
    ShoutWordData entry;
    entry.word = records_->ResolveFrom(bethesda::RawFormId{word_raw}, stored->winning_plugin).packed();
    entry.spell = records_->ResolveFrom(bethesda::RawFormId{spell_raw}, stored->winning_plugin).packed();
    std::memcpy(&entry.recovery, sub.data.data() + 8, 4);
    shout_word_cache_.push_back(entry);
  }
  return static_cast<i32>(shout_word_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthShoutWord(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= shout_word_cache_.size()) return {};
  return papyrus::ObjectRef{shout_word_cache_[static_cast<size_t>(index)].word};
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthShoutSpell(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= shout_word_cache_.size()) return {};
  return papyrus::ObjectRef{shout_word_cache_[static_cast<size_t>(index)].spell};
}

f32 RecordBackedSkyrimBindings::GetNthShoutRecoveryTime(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= shout_word_cache_.size()) return 0.0f;
  return shout_word_cache_[static_cast<size_t>(index)].recovery;
}

i32 RecordBackedSkyrimBindings::GetNthMagicEffectDuration(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= magic_effect_cache_.size()) return 0;
  return magic_effect_cache_[static_cast<size_t>(index)].duration;
}

std::string RecordBackedSkyrimBindings::GetMagicEffectActorValue(ObjectRef effect) {
  if (!records_) return "";
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(effect), &rec)) return "";
  if (rec.header.type != FourCc('M', 'G', 'E', 'F')) return "";
  // MGEF DATA holds the primary actor value the effect modifies at offset 0x44.
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 0x48) return "";
  i32 av;
  std::memcpy(&av, data->data.data() + 0x44, 4);
  return EffectAvName(av);
}

bool RecordBackedSkyrimBindings::GetMagicEffectDetrimental(ObjectRef effect) {
  if (!records_) return false;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(effect), &rec)) return false;
  if (rec.header.type != FourCc('M', 'G', 'E', 'F')) return false;
  // The 0x04 flag in MGEF DATA marks a detrimental effect (damage rather than
  // restore or fortify).
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 4) return false;
  u32 flags;
  std::memcpy(&flags, data->data.data(), 4);
  return (flags & 0x04) != 0;
}

f32 RecordBackedSkyrimBindings::GetMagicEffectBaseCost(ObjectRef effect) {
  if (!records_) return 0.0f;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(effect), &rec)) return 0.0f;
  if (rec.header.type != FourCc('M', 'G', 'E', 'F')) return 0.0f;
  // MGEF DATA holds the base cost (the effect's unit price) as a float at 0x04,
  // right after the flags. It anchors the magicka cost of a spell and the gold
  // value of a brewed potion that carries the effect.
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 0x08) return 0.0f;
  f32 cost;
  std::memcpy(&cost, data->data.data() + 0x04, 4);
  return cost;
}

namespace {
// Reads a uint32 from a spell's SPIT subrecord at `offset`. SPIT is { uint32 cost;
// uint32 flags; uint32 type; float chargeTime; uint32 castType; uint32 delivery;
// ... }, so cost is at 0, type at 8, cast type at 16 and delivery at 20.
i32 SpitField(const bethesda::RecordStore* records, bethesda::GlobalFormId id, size_t offset) {
  if (!records) return 0;
  bethesda::Record rec;
  if (!records->Parse(id, &rec)) return 0;
  if (rec.header.type != FourCc('S', 'P', 'E', 'L')) return 0;
  const bethesda::Subrecord* spit = rec.Find(FourCc('S', 'P', 'I', 'T'));
  if (!spit || spit->data.size() < offset + 4) return 0;
  u32 value;
  std::memcpy(&value, spit->data.data() + offset, 4);
  return static_cast<i32>(value);
}
}  // namespace

i32 RecordBackedSkyrimBindings::GetKeywordCount(ObjectRef form) {
  keyword_cache_.clear();
  if (!records_) return 0;
  const bethesda::GlobalFormId id = ToFormId(form);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return 0;
  // KWDA is a packed array of keyword form ids (KSIZ gives the count, but the
  // subrecord size already implies it). Each is local to the form's plugin.
  const bethesda::Subrecord* kwda = rec.Find(FourCc('K', 'W', 'D', 'A'));
  if (!kwda) return 0;
  for (size_t offset = 0; offset + 4 <= kwda->data.size(); offset += 4) {
    u32 raw;
    std::memcpy(&raw, kwda->data.data() + offset, 4);
    keyword_cache_.push_back(
        records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed());
  }
  return static_cast<i32>(keyword_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthKeyword(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= keyword_cache_.size()) return {};
  return papyrus::ObjectRef{keyword_cache_[static_cast<size_t>(index)]};
}

i32 RecordBackedSkyrimBindings::GetRaceSpellCount(ObjectRef race) {
  race_spell_cache_.clear();
  if (!records_) return 0;
  const bethesda::GlobalFormId id = ToFormId(race);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec) || rec.header.type != FourCc('R', 'A', 'C', 'E')) return 0;
  // SPLO is a single spell form id (the race's innate abilities and powers).
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type != FourCc('S', 'P', 'L', 'O') || sub.data.size() < 4) continue;
    u32 raw;
    std::memcpy(&raw, sub.data.data(), 4);
    race_spell_cache_.push_back(
        records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed());
  }
  return static_cast<i32>(race_spell_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthRaceSpell(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= race_spell_cache_.size()) return {};
  return papyrus::ObjectRef{race_spell_cache_[static_cast<size_t>(index)]};
}

i32 RecordBackedSkyrimBindings::GetRaceSkillBonusCount(ObjectRef race) {
  race_skill_cache_.clear();
  if (!records_) return 0;
  bethesda::Record rec;
  if (!records_->Parse(ToFormId(race), &rec) || rec.header.type != FourCc('R', 'A', 'C', 'E')) return 0;
  // RACE DATA opens with seven { uint8 skill-AV; uint8 bonus } pairs; an unused
  // slot has skill 255.
  const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 14) return 0;
  for (int i = 0; i < 7; ++i) {
    u8 skill = data->data[static_cast<size_t>(i) * 2];
    u8 bonus = data->data[static_cast<size_t>(i) * 2 + 1];
    if (skill == 255 || bonus == 0) continue;
    race_skill_cache_.push_back({skill, bonus});
  }
  return static_cast<i32>(race_skill_cache_.size());
}

std::string RecordBackedSkyrimBindings::GetNthRaceSkillBonusSkill(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= race_skill_cache_.size()) return "";
  return SkillAvName(static_cast<u32>(race_skill_cache_[static_cast<size_t>(index)].first));
}

i32 RecordBackedSkyrimBindings::GetNthRaceSkillBonusValue(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= race_skill_cache_.size()) return 0;
  return race_skill_cache_[static_cast<size_t>(index)].second;
}

i32 RecordBackedSkyrimBindings::GetSpellCost(ObjectRef spell) {
  return SpitField(records_, ToFormId(spell), 0);
}

i32 RecordBackedSkyrimBindings::GetSpellType(ObjectRef spell) {
  return SpitField(records_, ToFormId(spell), 8);
}

i32 RecordBackedSkyrimBindings::GetSpellCastType(ObjectRef spell) {
  return SpitField(records_, ToFormId(spell), 16);
}

i32 RecordBackedSkyrimBindings::GetSpellDelivery(ObjectRef spell) {
  return SpitField(records_, ToFormId(spell), 20);
}

}  // namespace rx::script::skyrim
