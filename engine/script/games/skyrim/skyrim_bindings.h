#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bethesda/load_order.h"
#include "bethesda/strings.h"
#include "core/types.h"
#include "quest/quest_graph.h"
#include "quest/quest_system.h"
#include "script/games/skyrim/skyrim_natives.h"
#include "script/world_effect_sink.h"

namespace rec::script::papyrus {
class VirtualMachine;
}

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
// Also a quest::QuestActionSink: when a quest graph enters a node, its on-enter
// actions are dispatched back here (RunScriptFragment runs the Papyrus stage
// fragment). Imported quests only use the fragment action; the rest stay the
// base no-ops until native quests need them.
class RecordBackedSkyrimBindings : public SkyrimBindings, public quest::QuestActionSink {
 public:
  RecordBackedSkyrimBindings() = default;
  explicit RecordBackedSkyrimBindings(const bethesda::RecordStore* records) : records_(records) {}

  void set_records(const bethesda::RecordStore* records) { records_ = records; }
  void set_strings(const bethesda::StringTable* strings) { strings_ = strings; }
  void set_player(papyrus::ObjectRef player) { player_ = player; }
  // Sink for quest-driven world mutations (spawn/move/enable/delete + cleanup).
  // Set by the runtime; when null, those bindings only update logical state.
  void set_world_sink(WorldEffectSink* sink) { world_sink_ = sink; }

  // Replica mode (a multiplayer client): the server is authoritative for quests
  // and quest-driven world state, so the client's own scripts must not mutate
  // either -- it mirrors the server via QuestSystem::ApplyStatus and replicated
  // world commands instead. Quest definitions still load (for journal text); the
  // authoritative mutators just become no-ops. Host and single-player leave this
  // false and run quests normally.
  void set_replica_mode(bool replica) { replica_mode_ = replica; }
  bool replica_mode() const { return replica_mode_; }
  // The guest VM, so SetStage can run the quest's stage fragment. Set once on
  // the guest thread (the only caller of these bindings).
  void set_vm(papyrus::VirtualMachine* vm) { vm_ = vm; }
  // Registers the Papyrus function a quest runs when it reaches `stage` (from
  // the QUST VMAD fragments). Populated at quest attach.
  void SetStageFragment(u64 quest, i32 stage, std::string function);

  // The engine-side quest store these bindings delegate to. The runtime reads
  // it for the HUD / debugger / network and seeds it with QUST definitions; all
  // access happens on the guest thread, the bindings' only caller.
  quest::QuestSystem& quest_system() { return quest_system_; }
  const quest::QuestSystem& quest_system() const { return quest_system_; }

  papyrus::ObjectRef GetPlayer() override { return player_; }
  papyrus::ObjectRef GetForm(u32 form_id) override;

  // Form data, from records.
  u32 GetFormId(papyrus::ObjectRef form) override;
  i32 GetFormType(papyrus::ObjectRef form) override;
  std::string GetName(papyrus::ObjectRef form) override;
  bool HasKeyword(papyrus::ObjectRef form, papyrus::ObjectRef keyword) override;
  i32 GetSex(papyrus::ObjectRef actor_base) override;
  bool IsUnique(papyrus::ObjectRef actor_base) override;
  bool IsEssential(papyrus::ObjectRef actor_base) override;
  papyrus::ObjectRef GetRace(papyrus::ObjectRef actor_base) override;

  // Spatial state: authored placement from the REFR record, overridable by
  // SetPosition / MoveTo (the override store is the new system).
  f32 GetPositionX(papyrus::ObjectRef ref) override;
  f32 GetPositionY(papyrus::ObjectRef ref) override;
  f32 GetPositionZ(papyrus::ObjectRef ref) override;
  void SetPosition(papyrus::ObjectRef ref, f32 x, f32 y, f32 z) override;
  f32 GetDistance(papyrus::ObjectRef a, papyrus::ObjectRef b) override;
  void MoveTo(papyrus::ObjectRef ref, papyrus::ObjectRef target) override;
  void SetEnabled(papyrus::ObjectRef ref, bool enabled) override;
  bool IsDisabled(papyrus::ObjectRef ref) override;
  void Delete(papyrus::ObjectRef ref) override;
  papyrus::ObjectRef PlaceAtMe(papyrus::ObjectRef where, papyrus::ObjectRef base,
                               i32 count) override;
  papyrus::ObjectRef GetBaseObject(papyrus::ObjectRef ref) override;
  bool IsInterior(papyrus::ObjectRef cell) override;
  f32 GetCellWaterLevel(papyrus::ObjectRef cell) override;
  f32 GetScale(papyrus::ObjectRef ref) override;
  void SetScale(papyrus::ObjectRef ref, f32 scale) override;
  bool IsLocked(papyrus::ObjectRef ref) override;
  void SetLocked(papyrus::ObjectRef ref, bool locked) override;
  i32 GetLockLevel(papyrus::ObjectRef ref) override;
  void SetLockLevel(papyrus::ObjectRef ref, i32 level) override;
  i32 GetOpenState(papyrus::ObjectRef ref) override;
  void SetOpen(papyrus::ObjectRef ref, bool open) override;

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

  // Player controls (new system).
  void SetPlayerControl(i32 category, bool enabled) override;
  bool IsPlayerControlEnabled(i32 category) override;

  // Global variables: seeded from the GLOB record, overridable.
  f32 GetGlobalValue(papyrus::ObjectRef global) override;
  void SetGlobalValue(papyrus::ObjectRef global, f32 value) override;

  // Actor values (new system): permanent base + damageable current.
  f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) override;
  f32 GetBaseActorValue(papyrus::ObjectRef actor, const std::string& av) override;
  f32 GetActorValuePercentage(papyrus::ObjectRef actor, const std::string& av) override;
  void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) override;
  void ForceActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) override;
  void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) override;
  void RestoreActorValue(papyrus::ObjectRef actor, const std::string& av, f32 amount) override;
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

  // quest::QuestActionSink: the graph drives stage entry through here. State and
  // dedup still live in quest_system_ (SetStage records the stage before the
  // graph runs), so this only needs to run the stage's Papyrus fragment.
  void RunScriptFragment(u64 quest, i32 node, const std::string& fragment) override;

 private:
  struct ActorValue {
    f32 base = 0;
    f32 current = 0;
  };
  bethesda::GlobalFormId ToFormId(papyrus::ObjectRef ref) const;
  // Reads a 4-byte form-id subrecord off `from`'s record and resolves it
  // against the load order. Used for record fields that point at another form.
  papyrus::ObjectRef ResolveFormRef(papyrus::ObjectRef from, u32 subrecord_type);
  std::array<f32, 3> Position(papyrus::ObjectRef ref);
  ActorValue& Av(papyrus::ObjectRef actor, const std::string& av);

  const bethesda::RecordStore* records_ = nullptr;
  const bethesda::StringTable* strings_ = nullptr;
  papyrus::ObjectRef player_;
  std::unordered_map<u64, std::unordered_map<std::string, ActorValue>> actor_values_;
  std::unordered_map<u64, std::unordered_map<u64, i32>> inventory_;
  std::unordered_map<u64, std::array<f32, 3>> positions_;  // SetPosition/MoveTo overrides
  std::unordered_map<u64, f32> scales_;                    // SetScale overrides (default 1.0)
  struct LockState {
    bool locked = false;
    i32 level = 0;
  };
  std::unordered_map<u64, LockState> locks_;
  std::unordered_map<u64, bool> open_;
  std::unordered_map<u64, bool> disabled_;  // Disable() state, for IsDisabled
  WorldEffectSink* world_sink_ = nullptr;
  bool replica_mode_ = false;  // true on a multiplayer client; see set_replica_mode
  // The quest whose fragment is currently running; world mutations made during
  // it are attributed to it so QuestWorld can roll them back. 0 outside a
  // fragment (engine-driven, unattributed).
  u64 active_quest_ = 0;
  quest::QuestSystem quest_system_;
  // quest handle -> stage -> Papyrus fragment function name (from the QUST VMAD).
  std::unordered_map<u64, std::unordered_map<i32, std::string>> stage_fragments_;
  // The native graph for a quest plus its live traversal. Built lazily from the
  // quest's definition + fragments the first time it is started/advanced, so by
  // then both are registered. Heap-held so the instance's graph pointer is
  // stable; dropped on ResetQuest to rebuild fresh.
  struct QuestRuntime {
    quest::QuestGraph graph;
    quest::QuestInstance instance;
    explicit QuestRuntime(quest::QuestGraph g)
        : graph(std::move(g)), instance(&graph) {}
  };
  std::unordered_map<u64, std::unique_ptr<QuestRuntime>> quest_runtime_;
  QuestRuntime& Runtime(u64 quest);
  papyrus::VirtualMachine* vm_ = nullptr;
  int fragment_depth_ = 0;  // guards stage->fragment->SetStage recursion
  // Runs the fragment for a freshly-set stage, if one is registered.
  void RunStageFragment(papyrus::ObjectRef quest, i32 stage);

  // Raises a Skyrim form event (OnDeath, OnItemAdded, ...) on the target form's
  // script instance, if it defines a handler -- a silent no-op without a VM or
  // handler. Runs synchronously on the guest thread (the bindings' only caller),
  // so a handler that mutates state is visible immediately to the caller.
  void RaiseFormEvent(u64 target, const char* event, std::vector<papyrus::Value> args);
  // Fires OnDying/OnDeath once when an actor's health first reaches zero, and
  // re-arms if it is healed back above zero.
  void MaybeNotifyDeath(papyrus::ObjectRef actor);
  std::unordered_set<u64> dead_;  // actors that have already announced OnDeath
  std::unordered_map<u64, std::unordered_map<u64, i32>> faction_ranks_;  // actor -> faction -> rank
  std::unordered_map<u64, std::unordered_map<u64, i32>> reactions_;      // faction -> other
  std::unordered_map<u64, i32> crime_gold_;                             // faction -> gold
  std::array<bool, SkyrimBindings::kControlCount> player_controls_{};   // true = enabled
  bool player_controls_init_ = false;
  std::unordered_map<u64, f32> global_values_;  // GlobalVariable overrides
};

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
