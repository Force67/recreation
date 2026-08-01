#include "components/script/games/skyrim/skyrim_native_state.h"

#include <base/containers/set.h>
#include <base/containers/unordered_map.h>
#include <base/strings/xstring.h>

namespace rx::script::skyrim::state {
namespace {

// Each store maps an owner handle to its named values. A composite key would
// work too, but the nested map keeps Clear(owner) a single erase.
template <typename T>
using Store = base::UnorderedMap<u64, base::UnorderedMap<base::String, T>>;

Store<bool>& Flags() {
  static Store<bool> s;
  return s;
}
Store<i32>& Ints() {
  static Store<i32> s;
  return s;
}
Store<f32>& Floats() {
  static Store<f32> s;
  return s;
}
Store<ObjectRef>& Refs() {
  static Store<ObjectRef> s;
  return s;
}
Store<base::Set<u64>>& Members() {
  static Store<base::Set<u64>> s;
  return s;
}

template <typename T>
T Get(const Store<T>& store, ObjectRef owner, const base::String& key, T fallback) {
  auto* o = store.find(owner.handle);
  if (o == nullptr) return fallback;
  auto* k = o->find(key);
  return k == nullptr ? fallback : *k;
}

}  // namespace

bool GetFlag(ObjectRef owner, const base::String& key, bool fallback) {
  return Get(Flags(), owner, key, fallback);
}
void SetFlag(ObjectRef owner, const base::String& key, bool value) {
  Flags()[owner.handle][key] = value;
}

i32 GetInt(ObjectRef owner, const base::String& key, i32 fallback) {
  return Get(Ints(), owner, key, fallback);
}
void SetInt(ObjectRef owner, const base::String& key, i32 value) {
  Ints()[owner.handle][key] = value;
}

f32 GetFloat(ObjectRef owner, const base::String& key, f32 fallback) {
  return Get(Floats(), owner, key, fallback);
}
void SetFloat(ObjectRef owner, const base::String& key, f32 value) {
  Floats()[owner.handle][key] = value;
}

ObjectRef GetRef(ObjectRef owner, const base::String& key) {
  return Get(Refs(), owner, key, ObjectRef{});
}
void SetRef(ObjectRef owner, const base::String& key, ObjectRef value) {
  Refs()[owner.handle][key] = value;
}

bool HasMember(ObjectRef owner, const base::String& key, ObjectRef member) {
  auto* o = Members().find(owner.handle);
  if (o == nullptr) return false;
  auto* k = o->find(key);
  return k != nullptr && k->count(member.handle) != 0;
}
void AddMember(ObjectRef owner, const base::String& key, ObjectRef member) {
  Members()[owner.handle][key].insert(member.handle);
}
void RemoveMember(ObjectRef owner, const base::String& key, ObjectRef member) {
  auto* o = Members().find(owner.handle);
  if (o == nullptr) return;
  auto* k = o->find(key);
  if (k != nullptr) k->erase(member.handle);
}
i32 MemberCount(ObjectRef owner, const base::String& key) {
  auto* o = Members().find(owner.handle);
  if (o == nullptr) return 0;
  auto* k = o->find(key);
  return k == nullptr ? 0 : static_cast<i32>(k->size());
}

void Clear(ObjectRef owner) {
  Flags().erase(owner.handle);
  Ints().erase(owner.handle);
  Floats().erase(owner.handle);
  Refs().erase(owner.handle);
  Members().erase(owner.handle);
}

}  // namespace rx::script::skyrim::state
