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

}  // namespace rec::script::skyrim
