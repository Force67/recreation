#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_

#include <string>
#include <unordered_map>

#include "bethesda/load_order.h"
#include "bethesda/strings.h"
#include "core/types.h"
#include "script/games/skyrim/skyrim_natives.h"

namespace rec::script::skyrim {

// A concrete SkyrimBindings that backs the native surface with real state:
//   - Form data (id, type, name, keywords) comes from the engine's existing
//     RecordStore + StringTable (the "implement with the current engine" half).
//   - Actor values and inventory are new in-engine stores written here, since
//     the engine has no such systems yet (the "write new systems" half).
//
// Instances are keyed by a packed form id (plugin << 32 | local), so an object
// property pointing at a form resolves to the same actor-value/inventory bucket.
// One instance per game world; the guest thread is its only caller, so it needs
// no internal locking.
class RecordBackedSkyrimBindings : public SkyrimBindings {
 public:
  RecordBackedSkyrimBindings() = default;
  explicit RecordBackedSkyrimBindings(const bethesda::RecordStore* records) : records_(records) {}

  void set_records(const bethesda::RecordStore* records) { records_ = records; }
  void set_strings(const bethesda::StringTable* strings) { strings_ = strings; }
  void set_player(papyrus::ObjectRef player) { player_ = player; }

  papyrus::ObjectRef GetPlayer() override { return player_; }

  // Form data, from records.
  u32 GetFormId(papyrus::ObjectRef form) override;
  i32 GetFormType(papyrus::ObjectRef form) override;
  std::string GetName(papyrus::ObjectRef form) override;
  bool HasKeyword(papyrus::ObjectRef form, papyrus::ObjectRef keyword) override;

  // Actor values (new system).
  f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) override;
  void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) override;
  void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) override;
  bool IsDead(papyrus::ObjectRef actor) override;

  // Inventory (new system).
  i32 GetItemCount(papyrus::ObjectRef container, papyrus::ObjectRef item) override;
  void AddItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) override;
  void RemoveItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) override;

 private:
  bethesda::GlobalFormId ToFormId(papyrus::ObjectRef ref) const;

  const bethesda::RecordStore* records_ = nullptr;
  const bethesda::StringTable* strings_ = nullptr;
  papyrus::ObjectRef player_;
  std::unordered_map<u64, std::unordered_map<std::string, f32>> actor_values_;
  std::unordered_map<u64, std::unordered_map<u64, i32>> inventory_;
};

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
