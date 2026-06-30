#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bethesda/load_order.h"
#include "bethesda/script_attachment.h"
#include "bethesda/strings.h"
#include "core/types.h"
#include "core/world_clock.h"
#include "quest/quest_graph.h"
#include "quest/scene_player.h"
#include "quest/quest_system.h"
#include "script/games/skyrim/skyrim_natives.h"
#include "script/host/bridge.h"
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
  // The in-world clock backing the time natives (Utility.GetCurrentGameTime and
  // the GameHour/GameDaysPassed/TimeScale globals). Owned by the runtime, which
  // advances it on the main thread; reads/writes here happen on the guest
  // thread, so the clock keeps its shared fields atomic. Null leaves the time
  // natives at their neutral defaults.
  void set_clock(WorldClock* clock) { clock_ = clock; }
  // The packed form handles of the time globals, resolved by editor id at load.
  // 0 means the game has no such global; that global then keeps record-authored
  // behaviour instead of routing to the clock.
  void set_time_globals(u64 game_hour, u64 game_days_passed, u64 timescale) {
    game_hour_global_ = game_hour;
    game_days_global_ = game_days_passed;
    timescale_global_ = timescale;
  }
  // Sink for quest-driven world mutations (spawn/move/enable/delete + cleanup).
  // Set by the runtime; when null, those bindings only update logical state.
  void set_world_sink(WorldEffectSink* sink) { world_sink_ = sink; }

  // Sink for gameplay events the managed (C#) world subscribes to (actor death,
  // item added, quest stage). Set by the runtime once the managed host is up;
  // when null, no events are emitted. Called on the guest thread, so the sink
  // must be thread-safe (the runtime enqueues for the main thread to drain).
  void set_event_sink(std::function<void(const host::ManagedEvent&)> sink) {
    event_sink_ = std::move(sink);
  }

  // Refreshes the world-space position snapshot the proximity query reads. Called
  // by the runtime each frame on the main thread; it takes the snapshot mutex, so
  // it is the one method on these bindings safe to call off the guest thread.
  void UpdatePositionSnapshot(const std::vector<std::pair<u64, std::array<f32, 3>>>& positions);

  // Replica mode (a multiplayer client): the server is authoritative for quests
  // and quest-driven world state, so the client's own scripts must not mutate
  // either, it mirrors the server via QuestSystem::ApplyStatus and replicated
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

  // Registers a scene's Papyrus fragments (from its SCEN VMAD): the begin/end and
  // per-phase functions that call SetStage as the scene plays. `scene` is the
  // scene form handle the SF_ script is attached to; `owning_quest` backs the
  // scene's GetOwningQuest() and attributes the fragment's world mutations.
  void SetSceneFragments(u64 scene, u64 owning_quest, bethesda::SceneFragments frags);
  // Runs a scene's begin/end fragment, or the fragment(s) registered for a phase
  // boundary (the begin or end of `phase`), via the VM. No-op without a VM or a
  // registered fragment. Driven by the runtime scene executor at the right moment.
  void RunSceneBegin(u64 scene);
  void RunSceneEnd(u64 scene);
  void RunScenePhase(u64 scene, u32 phase, bool on_begin);

  // Advances every scene the ScenePlayer is playing by `dt`, firing the phase
  // begin/end cues whose timers elapse. The runtime posts this each frame on the
  // guest thread; cheap when nothing is playing.
  void TickScenes(f32 dt);
  bool AnyScenePlaying() const { return scene_player_.playing_count() > 0; }
  // How many scene begin-fragments have run (scenes that have played). Lets a
  // driver confirm the Scene.Start path actually fired.
  u32 scenes_begun() const { return scenes_begun_; }

  // The engine-side quest store these bindings delegate to. The runtime reads
  // it for the HUD / debugger / network and seeds it with QUST definitions; all
  // access happens on the guest thread, the bindings' only caller.
  quest::QuestSystem& quest_system() { return quest_system_; }
  const quest::QuestSystem& quest_system() const { return quest_system_; }

  // Managed HUD gauges (Hud.Gauge native): named persistent bars the gameplay
  // systems push from the guest thread; the runtime snapshots them onto the HUD
  // on the main thread, so the store is mutex-guarded like live_positions_.
  struct HudGauge {
    std::string id;
    std::string label;
    f32 fraction = 0;
    u32 color = 0;  // packed rgba8; 0 = HUD default
  };
  void SetHudGauge(const std::string& id, f32 fraction, const std::string& label,
                   u32 color) override;
  void ClearHudGauge(const std::string& id) override;
  void SnapshotHudGauges(std::vector<HudGauge>& out) const;

  // War-map state (a hold's name + owner), pushed by managed code and snapshotted
  // onto the war-map panel. Mutex-guarded like the gauges.
  struct WarHold {
    std::string name;
    i32 owner = 0;  // 0 neutral, 1 Imperial, 2 Stormcloak
  };
  void SetWarHold(i32 index, const std::string& name, i32 owner) override;
  void SetWarProgress(f32 imperial_fraction) override;
  void SnapshotWarMap(std::vector<WarHold>& out, f32& imperial_fraction) const;

  // Applies a server-replicated quest status on a multiplayer client and, when it
  // carries a fresh stage, fires the managed QuestStageChanged event so C# mods
  // (XP rewards, the journal) react to the host's progress exactly as a local
  // SetStage would. SetStage itself short-circuits in replica mode, so without
  // this a replicated advance would never reach the managed layer.
  void ApplyReplicatedStatus(const quest::QuestStatus& status);

  papyrus::ObjectRef GetPlayer() override { return player_; }
  papyrus::ObjectRef GetForm(u32 form_id) override;

  // The reference an alias handle is filled with: a script-set runtime fill
  // (ForceRefTo) wins, else the authored forced-reference (ALFR) or unique-actor
  // (ALUA) rule, else None. Backs ReferenceAlias.GetReference/GetActorRef so a
  // quest fragment's alias properties resolve to real refs. See alias_handle.h.
  papyrus::ObjectRef AliasReference(papyrus::ObjectRef alias) override;
  void AliasForceRefTo(papyrus::ObjectRef alias, papyrus::ObjectRef ref) override;
  void AliasClear(papyrus::ObjectRef alias) override;

  // Form data, from records.
  u32 GetFormId(papyrus::ObjectRef form) override;
  i32 GetFormType(papyrus::ObjectRef form) override;
  std::string GetName(papyrus::ObjectRef form) override;
  bool HasKeyword(papyrus::ObjectRef form, papyrus::ObjectRef keyword) override;
  i32 GetKeywordCount(papyrus::ObjectRef form) override;
  papyrus::ObjectRef GetNthKeyword(i32 index) override;
  papyrus::ObjectRef GetHarvestIngredient(papyrus::ObjectRef flora) override;
  papyrus::ObjectRef GetBookSpell(papyrus::ObjectRef book) override;
  std::string GetBookSkill(papyrus::ObjectRef book) override;
  f32 GetWeight(papyrus::ObjectRef form) override;
  i32 GetGoldValue(papyrus::ObjectRef form) override;
  i32 GetWeaponDamage(papyrus::ObjectRef weapon) override;
  f32 GetArmorRating(papyrus::ObjectRef armor) override;
  papyrus::ObjectRef GetEnchantment(papyrus::ObjectRef item) override;
  i32 GetSoulGemSoul(papyrus::ObjectRef gem) override;
  i32 GetSoulGemCapacity(papyrus::ObjectRef gem) override;
  i32 GetMagicEffectCount(papyrus::ObjectRef item) override;
  papyrus::ObjectRef GetNthMagicEffectId(i32 index) override;
  f32 GetNthMagicEffectMagnitude(i32 index) override;
  i32 GetNthMagicEffectDuration(i32 index) override;
  i32 GetShoutWordCount(papyrus::ObjectRef shout) override;
  papyrus::ObjectRef GetNthShoutWord(i32 index) override;
  papyrus::ObjectRef GetNthShoutSpell(i32 index) override;
  f32 GetNthShoutRecoveryTime(i32 index) override;
  std::string GetMagicEffectActorValue(papyrus::ObjectRef effect) override;
  bool GetMagicEffectDetrimental(papyrus::ObjectRef effect) override;
  f32 GetMagicEffectBaseCost(papyrus::ObjectRef effect) override;
  i32 GetSpellCost(papyrus::ObjectRef spell) override;
  i32 GetSpellType(papyrus::ObjectRef spell) override;
  i32 GetSpellCastType(papyrus::ObjectRef spell) override;
  i32 GetSpellDelivery(papyrus::ObjectRef spell) override;
  i32 GetRecipeCount() override;
  papyrus::ObjectRef GetNthRecipeOutput(i32 recipe) override;
  i32 GetNthRecipeOutputQuantity(i32 recipe) override;
  papyrus::ObjectRef GetNthRecipeWorkbench(i32 recipe) override;
  i32 GetNthRecipeInputCount(i32 recipe) override;
  papyrus::ObjectRef GetNthRecipeInput(i32 recipe, i32 input) override;
  i32 GetNthRecipeInputQuantity(i32 recipe, i32 input) override;
  i32 GetLeveledListCount(papyrus::ObjectRef list) override;
  i32 GetLeveledChanceNone() override;
  i32 GetLeveledFlags() override;
  papyrus::ObjectRef GetNthLeveledForm(i32 index) override;
  i32 GetNthLeveledLevel(i32 index) override;
  i32 GetNthLeveledCount(i32 index) override;
  i32 GetSex(papyrus::ObjectRef actor_base) override;
  bool IsUnique(papyrus::ObjectRef actor_base) override;
  bool IsEssential(papyrus::ObjectRef actor_base) override;
  papyrus::ObjectRef GetRace(papyrus::ObjectRef actor_base) override;
  i32 GetRaceSpellCount(papyrus::ObjectRef race) override;
  papyrus::ObjectRef GetNthRaceSpell(i32 index) override;
  i32 GetRaceSkillBonusCount(papyrus::ObjectRef race) override;
  std::string GetNthRaceSkillBonusSkill(i32 index) override;
  i32 GetNthRaceSkillBonusValue(i32 index) override;

  i32 GetNearbyRefs(papyrus::ObjectRef center, f32 radius) override;
  papyrus::ObjectRef GetNthNearbyRef(i32 index) override;
  f32 GetNthNearbyDistance(i32 index) override;

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
  i32 GetFactionCount(papyrus::ObjectRef actor) override;
  papyrus::ObjectRef GetNthFaction(i32 index) override;
  i32 GetNthFactionRank(i32 index) override;
  i32 GetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  void SetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction, i32 rank) override;
  bool IsInFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  void AddToFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  void RemoveFromFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) override;
  i32 GetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other) override;
  i32 GetFactionFlags(papyrus::ObjectRef faction) override;
  void SetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other, i32 reaction) override;
  i32 GetCrimeGold(papyrus::ObjectRef faction) override;
  void SetCrimeGold(papyrus::ObjectRef faction, i32 gold) override;
  void ModCrimeGold(papyrus::ObjectRef faction, i32 delta) override;

  // Player controls (new system).
  void SetPlayerControl(i32 category, bool enabled) override;
  bool IsPlayerControlEnabled(i32 category) override;

  // Global variables: seeded from the GLOB record, overridable. The time
  // globals (GameHour/GameDaysPassed/TimeScale) read and write the world clock.
  f32 GetGlobalValue(papyrus::ObjectRef global) override;
  void SetGlobalValue(papyrus::ObjectRef global, f32 value) override;

  // Time, backed by the world clock (neutral defaults without one).
  f32 GetCurrentGameTime() override;  // days since start (GameDaysPassed)
  f32 GetRealHoursPassed() override;  // real wall-clock hours since load

  // Actor values (new system): permanent base + damageable current.
  f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) override;
  f32 GetBaseActorValue(papyrus::ObjectRef actor, const std::string& av) override;
  f32 GetActorValuePercentage(papyrus::ObjectRef actor, const std::string& av) override;
  void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) override;
  void ForceActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) override;
  void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) override;
  void RestoreActorValue(papyrus::ObjectRef actor, const std::string& av, f32 amount) override;
  bool IsDead(papyrus::ObjectRef actor) override;
  void Resurrect(papyrus::ObjectRef actor) override;
  bool IsInCombat(papyrus::ObjectRef actor) override;
  papyrus::ObjectRef GetCombatTarget(papyrus::ObjectRef actor) override;
  void StartCombat(papyrus::ObjectRef actor, papyrus::ObjectRef target) override;
  void StopCombat(papyrus::ObjectRef actor) override;
  // Applies one connected melee swing: removes `damage` health from `target` and
  // attributes the death to `attacker` if it falls. Called by the engine's
  // combat driver (main thread) via the guest queue, so it runs where the actor
  // values live. A no-op on an already-dead or invalid target.
  void ApplyMeleeHit(papyrus::ObjectRef attacker, papyrus::ObjectRef target, f32 damage);

  // Expands the <Alias=Name> and <Global=Name> tokens in a quest's display text
  // against live state, so an objective like "Battle for <Alias=City>" reads
  // "Battle for Whiterun" and "<Global=CWPercentPoolRemainingAttacker>" shows the
  // number. Unknown tokens are left in place. Guest-thread only (touches the
  // quest system + records). Returns raw unchanged when it carries no token.
  std::string ResolveQuestText(u64 quest, const std::string& raw);

  // Inventory (new system).
  i32 GetItemCount(papyrus::ObjectRef container, papyrus::ObjectRef item) override;
  void AddItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) override;
  void RemoveItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) override;
  i32 GetNumItems(papyrus::ObjectRef container) override;
  papyrus::ObjectRef GetNthForm(papyrus::ObjectRef container, i32 index) override;

  // Quests (new system): stage, running state, objectives.
  i32 GetStage(papyrus::ObjectRef quest) override;
  void SetStage(papyrus::ObjectRef quest, i32 stage) override;
  bool GetStageDone(papyrus::ObjectRef quest, i32 stage) override;
  std::string GetJournalEntry(papyrus::ObjectRef quest) override;
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
  papyrus::ObjectRef SceneOwningQuest(papyrus::ObjectRef scene) override;
  // A dialogue INFO's owning quest, so a TIF_ fragment's GetOwningQuest().SetStage
  // resolves. Registered when the fragment is fired (the dialogue layer knows the
  // topic's quest), mirroring the scene fragments' owning quest.
  void SetInfoOwningQuest(u64 info, u64 quest) { info_owning_quest_[info] = quest; }
  papyrus::ObjectRef InfoOwningQuest(papyrus::ObjectRef info) override;
  void SceneStart(papyrus::ObjectRef scene) override;
  void SceneStop(papyrus::ObjectRef scene) override;
  bool SceneIsPlaying(papyrus::ObjectRef scene) override;

  // quest::QuestActionSink: the graph drives stage entry through here. State and
  // dedup still live in quest_system_ (SetStage records the stage before the
  // graph runs), so this only needs to run the stage's Papyrus fragment.
  void RunScriptFragment(u64 quest, i32 node, const std::string& fragment) override;

 private:
  struct ActorValue {
    f32 base = 0;
    f32 current = 0;
  };
  // One parsed magic effect (INGR/ALCH EFID/EFIT): the effect's global form handle
  // and its base magnitude/duration, cached by GetMagicEffectCount.
  struct MagicEffectData {
    u64 effect = 0;
    f32 magnitude = 0;
    i32 duration = 0;
  };
  // One word of a shout (SHOU SNAM): the word of power, the spell it casts, and
  // the seconds before the shout can be used again. Cached by GetShoutWordCount.
  struct ShoutWordData {
    u64 word = 0;
    u64 spell = 0;
    f32 recovery = 0;
  };
  // A constructible-object recipe (COBJ), parsed once into recipe_cache_.
  struct CraftingInput {
    u64 item = 0;
    i32 count = 0;
  };
  struct Recipe {
    u64 output = 0;
    i32 output_count = 1;
    u64 workbench = 0;  // BNAM keyword (the station type that crafts it)
    std::vector<CraftingInput> inputs;
  };
  void BuildRecipes();  // fills recipe_cache_ from every COBJ record
  const Recipe* RecipeAt(i32 index) const;  // bounds-checked, nullptr if out of range
  // A leveled list (LVLI) parsed into the cache GetLeveledListCount fills.
  struct LeveledEntry {
    u64 form = 0;
    i32 level = 0;
    i32 count = 0;
  };
  struct LeveledList {
    i32 chance_none = 0;
    i32 flags = 0;
    std::vector<LeveledEntry> entries;
  };
  bethesda::GlobalFormId ToFormId(papyrus::ObjectRef ref) const;
  // Reads a 4-byte form-id subrecord off `from`'s record and resolves it
  // against the load order. Used for record fields that point at another form.
  papyrus::ObjectRef ResolveFormRef(papyrus::ObjectRef from, u32 subrecord_type);
  // The rank an actor is authored into `faction` (its NPC_ SNAM), or -2 if it is
  // not authored into it. Backs GetFactionRank when no runtime override is set.
  i32 AuthoredFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction);
  // The combat reaction `faction` is authored to hold toward `other` (its FACT
  // XNAM): 0 neutral, 1 enemy, 2 ally, 3 friend. Backs GetReaction when no runtime
  // override is set.
  i32 AuthoredFactionReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other);
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
  std::unordered_map<u64, u64> alias_fills_;  // alias handle -> ForceRefTo override ref
  // Reverse index: a filled ref -> the alias handles it fills, so a dying actor's
  // death dispatches to its alias scripts (CWReinforcementAliasScript.OnDeath).
  std::unordered_map<u64, std::vector<u64>> ref_to_aliases_;
  // Drops the ref -> alias reverse link when an alias is refilled or cleared.
  void EraseRefAlias(u64 ref, u64 alias_handle);
  WorldEffectSink* world_sink_ = nullptr;
  std::function<void(const host::ManagedEvent&)> event_sink_;  // managed event bus, see above
  // Emits a gameplay event to the managed world, if a sink is set.
  void EmitManagedEvent(host::ManagedEventId id, u64 a, u64 b, i32 i);
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

  // A scene's registered fragments plus the quest they advance. Keyed by scene
  // form handle (the SF_ script's instance), built at scene attach.
  struct SceneFragmentSet {
    u64 owning_quest = 0;
    bethesda::SceneFragments frags;
  };
  std::unordered_map<u64, SceneFragmentSet> scene_fragments_;
  std::unordered_map<u64, u64> info_owning_quest_;  // INFO handle -> owning quest
  // Runs one scene fragment function via the VM, attributed to its owning quest.
  void RunSceneFragment(u64 scene, u64 owning_quest, const std::string& function);
  // Plays the scenes a quest fragment Started, firing their phase fragments over
  // time. Lives here (guest thread) so the Scene.Start native can reach it.
  quest::ScenePlayer scene_player_;
  u32 scenes_begun_ = 0;  // count of scene begin-fragments run (diagnostics)

  // Raises a Skyrim form event (OnDeath, OnItemAdded, ...) on the target form's
  // script instance, if it defines a handler, a silent no-op without a VM or
  // handler. Runs synchronously on the guest thread (the bindings' only caller),
  // so a handler that mutates state is visible immediately to the caller.
  void RaiseFormEvent(u64 target, const char* event, std::vector<papyrus::Value> args);
  // Fires OnDying/OnDeath once when an actor's health first reaches zero, and
  // re-arms if it is healed back above zero.
  void MaybeNotifyDeath(papyrus::ObjectRef actor);
  std::unordered_set<u64> dead_;  // actors that have already announced OnDeath
  std::unordered_map<u64, u64> combat_target_;  // attacker -> target it is fighting
  // The actor whose swing is currently being resolved, so MaybeNotifyDeath can
  // attribute the kill (OnDeath's killer arg, the kActorDied event's b field).
  papyrus::ObjectRef last_attacker_{0};
  std::unordered_map<u64, std::unordered_map<u64, i32>> faction_ranks_;  // actor -> faction -> rank
  // Last GetFactionCount result (authored faction handle + rank), read by the
  // GetNthFaction* accessors.
  std::vector<std::pair<u64, i32>> faction_cache_;
  std::unordered_map<u64, std::unordered_map<u64, i32>> reactions_;      // faction -> other
  std::unordered_map<u64, i32> crime_gold_;                             // faction -> gold
  std::array<bool, SkyrimBindings::kControlCount> player_controls_{};   // true = enabled
  bool player_controls_init_ = false;
  std::unordered_map<u64, f32> global_values_;  // GlobalVariable overrides
  // The day/night clock and the packed handles of the globals that proxy it.
  // Owned by the runtime; null/0 until wired (see set_clock/set_time_globals).
  WorldClock* clock_ = nullptr;
  u64 game_hour_global_ = 0;
  u64 game_days_global_ = 0;
  u64 timescale_global_ = 0;

  // Proximity query state. live_positions_ is a world-space snapshot the runtime
  // refreshes each frame (main thread); the natives read it on the guest thread,
  // so the mutex guards both it and the cached last result. This is the only
  // bindings state touched from two threads.
  std::mutex live_positions_mutex_;
  std::unordered_map<u64, std::array<f32, 3>> live_positions_;
  // Managed HUD gauges, id-keyed, insertion-ordered for a stable HUD layout.
  // Written on the guest thread (SetHudGauge), read on the main thread
  // (SnapshotHudGauges); guarded by its own mutex.
  mutable std::mutex hud_gauges_mutex_;
  std::vector<HudGauge> hud_gauges_;
  mutable std::mutex war_map_mutex_;
  std::vector<WarHold> war_holds_;
  f32 war_progress_ = 0.0f;
  std::vector<std::pair<u64, f32>> nearby_cache_;  // last result: (handle, distance in game units)
  // Last GetMagicEffectCount result, read by the GetNthMagicEffect* accessors.
  // Guest-thread only (record parsing), so it needs no lock.
  std::vector<MagicEffectData> magic_effect_cache_;
  // Last GetShoutWordCount result, read by the GetNthShout* accessors. Guest
  // thread only.
  std::vector<ShoutWordData> shout_word_cache_;
  // Last GetKeywordCount result (resolved keyword handles), read by GetNthKeyword.
  std::vector<u64> keyword_cache_;
  // Last GetRaceSpellCount result (resolved spell handles), read by GetNthRaceSpell.
  std::vector<u64> race_spell_cache_;
  // Last GetRaceSkillBonusCount result as (skill AV index, bonus) pairs.
  std::vector<std::pair<i32, i32>> race_skill_cache_;
  // Every COBJ recipe, built lazily on first GetRecipeCount and reused after.
  std::vector<Recipe> recipe_cache_;
  bool recipes_built_ = false;
  // Last leveled list parsed by GetLeveledListCount; the other LVLI accessors
  // read it. Guest-thread only, like the other record caches.
  LeveledList leveled_cache_;
};

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_BINDINGS_H_
