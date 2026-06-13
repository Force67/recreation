#include "script/games/skyrim/skyrim_bindings.h"

#include <algorithm>
#include <cctype>
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

f32 RecordBackedSkyrimBindings::GetActorValue(ObjectRef actor, const std::string& av) {
  auto actor_it = actor_values_.find(actor.handle);
  if (actor_it != actor_values_.end()) {
    auto av_it = actor_it->second.find(Lower(av));
    if (av_it != actor_it->second.end()) return av_it->second;
  }
  return DefaultActorValue(av);
}

void RecordBackedSkyrimBindings::SetActorValue(ObjectRef actor, const std::string& av, f32 value) {
  actor_values_[actor.handle][Lower(av)] = value;
}

void RecordBackedSkyrimBindings::ModActorValue(ObjectRef actor, const std::string& av, f32 delta) {
  f32 current = GetActorValue(actor, av);
  actor_values_[actor.handle][Lower(av)] = current + delta;
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

}  // namespace rec::script::skyrim
