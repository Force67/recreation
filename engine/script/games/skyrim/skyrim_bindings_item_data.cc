// Item record-data accessors for RecordBackedSkyrimBindings: weight, gold value,
// weapon damage and armor rating, parsed from the form's record. Split out of
// skyrim_bindings.cc to keep that file from becoming a god file; the rest of the
// class lives there and in the other skyrim_bindings_*.cc units.
#include "script/games/skyrim/skyrim_bindings.h"

#include <cstring>

#include "bethesda/record.h"

namespace rec::script::skyrim {
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

i32 RecordBackedSkyrimBindings::GetIngredientEffectCount(ObjectRef ingredient) {
  ingredient_effect_cache_.clear();
  if (!records_) return 0;
  const bethesda::GlobalFormId id = ToFormId(ingredient);
  const bethesda::RecordStore::StoredRecord* stored = records_->Find(id);
  if (!stored) return 0;
  bethesda::Record rec;
  if (!records_->Parse(id, &rec)) return 0;
  if (rec.header.type != FourCc('I', 'N', 'G', 'R')) return 0;

  // INGR lists up to four effects as ordered EFID (effect form id) / EFIT
  // ({ float magnitude; uint32 area; uint32 duration }) pairs. Each EFID's id is
  // resolved against the ingredient's load order so managed code gets a real
  // global handle it can compare across ingredients.
  IngredientEffect pending;
  bool have_effect = false;
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type == FourCc('E', 'F', 'I', 'D') && sub.data.size() >= 4) {
      u32 raw;
      std::memcpy(&raw, sub.data.data(), 4);
      pending = IngredientEffect{};
      pending.effect =
          records_->ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin).packed();
      have_effect = true;
    } else if (sub.type == FourCc('E', 'F', 'I', 'T') && have_effect && sub.data.size() >= 12) {
      std::memcpy(&pending.magnitude, sub.data.data(), 4);
      u32 duration;
      std::memcpy(&duration, sub.data.data() + 8, 4);
      pending.duration = static_cast<i32>(duration);
      ingredient_effect_cache_.push_back(pending);
      have_effect = false;
    }
  }
  return static_cast<i32>(ingredient_effect_cache_.size());
}

papyrus::ObjectRef RecordBackedSkyrimBindings::GetNthIngredientEffectId(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= ingredient_effect_cache_.size()) return {};
  return papyrus::ObjectRef{ingredient_effect_cache_[static_cast<size_t>(index)].effect};
}

f32 RecordBackedSkyrimBindings::GetNthIngredientEffectMagnitude(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= ingredient_effect_cache_.size()) return 0.0f;
  return ingredient_effect_cache_[static_cast<size_t>(index)].magnitude;
}

i32 RecordBackedSkyrimBindings::GetNthIngredientEffectDuration(i32 index) {
  if (index < 0 || static_cast<size_t>(index) >= ingredient_effect_cache_.size()) return 0;
  return ingredient_effect_cache_[static_cast<size_t>(index)].duration;
}

}  // namespace rec::script::skyrim
