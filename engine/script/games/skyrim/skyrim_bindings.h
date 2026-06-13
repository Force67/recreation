#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_

#include <array>
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

  // Spatial state: authored placement from the REFR record, overridable by
  // SetPosition / MoveTo (the override store is the new system).
  f32 GetPositionX(papyrus::ObjectRef ref) override;
  f32 GetPositionY(papyrus::ObjectRef ref) override;
  f32 GetPositionZ(papyrus::ObjectRef ref) override;
  void SetPosition(papyrus::ObjectRef ref, f32 x, f32 y, f32 z) override;
  f32 GetDistance(papyrus::ObjectRef a, papyrus::ObjectRef b) override;
  void MoveTo(papyrus::ObjectRef ref, papyrus::ObjectRef target) override;
  f32 GetScale(papyrus::ObjectRef ref) override;
  void SetScale(papyrus::ObjectRef ref, f32 scale) override;

  // Factions (new system): membership/rank, reactions, crime gold.
  i32 GetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  void SetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction, i32 rank) override;
  bool IsInFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  void AddToFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  void RemoveFromFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  i32 GetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other) override;
  void SetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other, i32 reaction) override;
  i32 GetCrimeGold(papyrus::ObjectRef faction) override;
  void SetCrimeGold(papyrus::ObjectRef faction, i32 gold) override;
  void ModCrimeGold(papyrus::ObjectRef faction, i32 delta) override;

  // Actor values (new system).
  f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) override;
  void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) override;
  void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) override;
  bool IsDead(papyrus::ObjectRef actor) override;

  // Inventory (new system).
  i32 GetItemCount(papyrus::ObjectRef container, papyrus::ObjectRef item) override;
  void AddItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) override;
  void RemoveItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) override;

  // Quests (new system): stage, running state, objectives.
  i32 GetStage(papyrus::ObjectRef quest) override;
  void SetStage(papyrus::ObjectRef quest, i32 stage) override;
  bool GetStageDone(papyrus::ObjectRef quest, i32 stage) override;
  bool IsRunning(papyrus::ObjectRef quest) override;
  void StartQuest(papyrus::ObjectRef quest) override;
  void StopQuest(papyrus::ObjectRef quest) override;
  void ResetQuest(papyrus::ObjectRef quest) override;
  bool IsQuestActive(papyrus::ObjectRef quest) override;
  void SetQuestActive(papyrus::ObjectRef quest, bool active) override;
  void SetObjectiveDisplayed(papyrus::ObjectRef quest, i32 objective, bool displayed) override;
  void SetObjectiveCompleted(papyrus::ObjectRef quest, i32 objective, bool completed) override;
  bool IsObjectiveDisplayed(papyrus::ObjectRef quest, i32 objective) override;
  bool IsObjectiveCompleted(papyrus::ObjectRef quest, i32 objective) override;

 private:
  struct QuestState {
    bool running = false;
    bool active = true;
    i32 stage = 0;
    std::unordered_map<i32, bool> stage_done;
    std::unordered_map<i32, bool> objective_displayed;
    std::unordered_map<i32, bool> objective_completed;
  };

  bethesda::GlobalFormId ToFormId(papyrus::ObjectRef ref) const;
  std::array<f32, 3> Position(papyrus::ObjectRef ref);

  const bethesda::RecordStore* records_ = nullptr;
  const bethesda::StringTable* strings_ = nullptr;
  papyrus::ObjectRef player_;
  std::unordered_map<u64, std::unordered_map<std::string, f32>> actor_values_;
  std::unordered_map<u64, std::unordered_map<u64, i32>> inventory_;
  std::unordered_map<u64, std::array<f32, 3>> positions_;  // SetPosition/MoveTo overrides
  std::unordered_map<u64, f32> scales_;                    // SetScale overrides (default 1.0)
  std::unordered_map<u64, QuestState> quests_;
  std::unordered_map<u64, std::unordered_map<u64, i32>> faction_ranks_;  // actor -> faction -> rank
  std::unordered_map<u64, std::unordered_map<u64, i32>> reactions_;      // faction -> other
  std::unordered_map<u64, i32> crime_gold_;                             // faction -> gold
};

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
