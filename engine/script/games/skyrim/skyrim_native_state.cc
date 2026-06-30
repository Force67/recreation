#include "script/games/skyrim/skyrim_native_state.h"

#include <set>
#include <unordered_map>

namespace rec::script::skyrim::state {
namespace {

// Each store maps an owner handle to its named values. A composite key would
// work too, but the nested map keeps Clear(owner) a single erase.
template <typename T>
using Store = std::unordered_map<u64, std::unordered_map<std::string, T>>;

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
Store<std::set<u64>>& Members() {
  static Store<std::set<u64>> s;
  return s;
}

template <typename T>
T Get(const Store<T>& store, ObjectRef owner, const std::string& key, T fallback) {
  auto o = store.find(owner.handle);
  if (o == store.end()) return fallback;
  auto k = o->second.find(key);
  return k == o->second.end() ? fallback : k->second;
}

}  // namespace

bool GetFlag(ObjectRef owner, const std::string& key, bool fallback) {
  return Get(Flags(), owner, key, fallback);
}
void SetFlag(ObjectRef owner, const std::string& key, bool value) {
  Flags()[owner.handle][key] = value;
}

i32 GetInt(ObjectRef owner, const std::string& key, i32 fallback) {
  return Get(Ints(), owner, key, fallback);
}
void SetInt(ObjectRef owner, const std::string& key, i32 value) {
  Ints()[owner.handle][key] = value;
}

f32 GetFloat(ObjectRef owner, const std::string& key, f32 fallback) {
  return Get(Floats(), owner, key, fallback);
}
void SetFloat(ObjectRef owner, const std::string& key, f32 value) {
  Floats()[owner.handle][key] = value;
}

ObjectRef GetRef(ObjectRef owner, const std::string& key) {
  return Get(Refs(), owner, key, ObjectRef{});
}
void SetRef(ObjectRef owner, const std::string& key, ObjectRef value) {
  Refs()[owner.handle][key] = value;
}

bool HasMember(ObjectRef owner, const std::string& key, ObjectRef member) {
  auto o = Members().find(owner.handle);
  if (o == Members().end()) return false;
  auto k = o->second.find(key);
  return k != o->second.end() && k->second.count(member.handle) != 0;
}
void AddMember(ObjectRef owner, const std::string& key, ObjectRef member) {
  Members()[owner.handle][key].insert(member.handle);
}
void RemoveMember(ObjectRef owner, const std::string& key, ObjectRef member) {
  auto o = Members().find(owner.handle);
  if (o == Members().end()) return;
  auto k = o->second.find(key);
  if (k != o->second.end()) k->second.erase(member.handle);
}
i32 MemberCount(ObjectRef owner, const std::string& key) {
  auto o = Members().find(owner.handle);
  if (o == Members().end()) return 0;
  auto k = o->second.find(key);
  return k == o->second.end() ? 0 : static_cast<i32>(k->second.size());
}

void Clear(ObjectRef owner) {
  Flags().erase(owner.handle);
  Ints().erase(owner.handle);
  Floats().erase(owner.handle);
  Refs().erase(owner.handle);
  Members().erase(owner.handle);
}

}  // namespace rec::script::skyrim::state
