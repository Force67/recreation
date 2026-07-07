#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_

#include <string>

#include "core/types.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"

namespace rec::script::skyrim {

// Everything the Skyrim native surface needs from the engine. The engine
// implements the slices it supports; the rest keep the defaults here, so a
// partially wired engine still runs scripts (they just see neutral values).
// The guest never sees an engine type, only this interface, which keeps the two
// worlds decoupled. A Fallout binding would be a separate interface paired with
// a separate native table.
class SkyrimBindings {
 public:
  virtual ~SkyrimBindings() = default;

  // Game
  virtual papyrus::ObjectRef GetPlayer() { return {}; }
  // Looks up a form by its runtime (load-order) form id.
  virtual papyrus::ObjectRef GetForm(u32 form_id) { return {}; }

  // Form
  virtual u32 GetFormId(papyrus::ObjectRef form) { return 0; }
  virtual i32 GetFormType(papyrus::ObjectRef form) { return 0; }
  virtual std::string GetName(papyrus::ObjectRef form) { return ""; }
  virtual bool HasKeyword(papyrus::ObjectRef form, papyrus::ObjectRef keyword) { return false; }
  // Item weight and gold value, read from the form's record. Supported for the
  // common inventory item types (weapons, armor, misc, ingredients, soul gems,
  // keys, potions); other forms report 0.
  virtual f32 GetWeight(papyrus::ObjectRef form) { return 0; }
  virtual i32 GetGoldValue(papyrus::ObjectRef form) { return 0; }
  // Base physical damage of a weapon form (WEAP); 0 for non-weapons.
  virtual i32 GetWeaponDamage(papyrus::ObjectRef weapon) { return 0; }
  // Base armor rating of an armor form (ARMO); 0 for non-armor.
  virtual f32 GetArmorRating(papyrus::ObjectRef armor) { return 0; }
  // The enchantment on a weapon or armor form (its EITM); None if unenchanted.
  virtual papyrus::ObjectRef GetEnchantment(papyrus::ObjectRef item) { return {}; }

  // The ingredient a harvestable flora (FLOR) produces, from its PFIG; None for
  // non-flora. The form passed is the flora base, not a placed reference.
  virtual papyrus::ObjectRef GetHarvestIngredient(papyrus::ObjectRef flora) { return {}; }

  // ActorBase (NPC_ record data).
  virtual i32 GetSex(papyrus::ObjectRef actor_base) { return 0; }  // 0 male, 1 female
  virtual bool IsUnique(papyrus::ObjectRef actor_base) { return false; }
  virtual bool IsEssential(papyrus::ObjectRef actor_base) { return false; }
  virtual papyrus::ObjectRef GetRace(papyrus::ObjectRef actor_base) { return {}; }

  // ObjectReference spatial state (engine transform / ECS).
  virtual f32 GetPositionX(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionY(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionZ(papyrus::ObjectRef ref) { return 0; }
  virtual void SetPosition(papyrus::ObjectRef ref, f32 x, f32 y, f32 z) {}
  virtual f32 GetDistance(papyrus::ObjectRef a, papyrus::ObjectRef b) { return 0; }
  virtual void MoveTo(papyrus::ObjectRef ref, papyrus::ObjectRef target) {}
  virtual void SetEnabled(papyrus::ObjectRef ref, bool enabled) {}
  virtual papyrus::ObjectRef GetBaseObject(papyrus::ObjectRef ref) { return {}; }
  // The reference a quest alias is filled with (ReferenceAlias.GetReference).
  virtual papyrus::ObjectRef AliasReference(papyrus::ObjectRef alias) { return {}; }
  // Runtime alias fill (ReferenceAlias.ForceRefTo / Clear): a script-set override
  // that takes precedence over the authored fill rule until cleared.
  virtual void AliasForceRefTo(papyrus::ObjectRef alias, papyrus::ObjectRef ref) {}
  virtual void AliasClear(papyrus::ObjectRef alias) {}

  // Cell data (CELL record).
  virtual bool IsInterior(papyrus::ObjectRef cell) { return false; }
  virtual f32 GetCellWaterLevel(papyrus::ObjectRef cell) { return 0; }
  virtual bool IsDisabled(papyrus::ObjectRef ref) { return false; }
  virtual i32 GetItemCount(papyrus::ObjectRef container, papyrus::ObjectRef item) { return 0; }
  virtual void AddItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) {}
  virtual void RemoveItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) {}
  // Inventory enumeration: the number of distinct item forms a container holds,
  // and the form at an index in [0, GetNumItems). The order is stable only while
  // the inventory is unchanged, so a listing loop must not add or remove items.
  virtual i32 GetNumItems(papyrus::ObjectRef container) { return 0; }
  virtual papyrus::ObjectRef GetNthForm(papyrus::ObjectRef container, i32 index) { return {}; }
  virtual void Activate(papyrus::ObjectRef target, papyrus::ObjectRef activator) {}
  virtual void Delete(papyrus::ObjectRef ref) {}
  virtual papyrus::ObjectRef PlaceAtMe(papyrus::ObjectRef where, papyrus::ObjectRef base,
                                       i32 count) {
    return {};
  }
  virtual f32 GetScale(papyrus::ObjectRef ref) { return 1.0f; }
  virtual void SetScale(papyrus::ObjectRef ref, f32 scale) {}
  // Lock and door state (new system).
  virtual bool IsLocked(papyrus::ObjectRef ref) { return false; }
  virtual void SetLocked(papyrus::ObjectRef ref, bool locked) {}
  virtual i32 GetLockLevel(papyrus::ObjectRef ref) { return 0; }
  virtual void SetLockLevel(papyrus::ObjectRef ref, i32 level) {}
  virtual i32 GetOpenState(papyrus::ObjectRef ref) { return 3; }  // 1 open, 3 closed
  virtual void SetOpen(papyrus::ObjectRef ref, bool open) {}

  // Actor values and state. The value has a permanent base and a damageable
  // current; percentage is current/base.
  virtual f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) { return 0; }
  virtual f32 GetBaseActorValue(papyrus::ObjectRef actor, const std::string& av) { return 0; }
  virtual f32 GetActorValuePercentage(papyrus::ObjectRef actor, const std::string& av) {
    return 1.0f;
  }
  virtual void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) {}
  virtual void ForceActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) {}
  virtual void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) {}
  virtual void RestoreActorValue(papyrus::ObjectRef actor, const std::string& av, f32 amount) {}
  virtual i32 GetLevel(papyrus::ObjectRef actor) { return 1; }
  virtual bool IsDead(papyrus::ObjectRef actor) { return false; }
  virtual bool IsInCombat(papyrus::ObjectRef actor) { return false; }
  virtual bool IsSneaking(papyrus::ObjectRef actor) { return false; }
  virtual papyrus::ObjectRef GetCombatTarget(papyrus::ObjectRef actor) { return {}; }
  virtual void EquipItem(papyrus::ObjectRef actor, papyrus::ObjectRef item) {}
  virtual void AddSpell(papyrus::ObjectRef actor, papyrus::ObjectRef spell) {}

  // Faction membership (actor side) and faction-wide reaction / crime state.
  virtual i32 GetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction) { return -2; }
  virtual void SetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction, i32 rank) {}
  virtual bool IsInFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) { return false; }
  virtual void AddToFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) {}
  virtual void RemoveFromFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) {}
  virtual i32 GetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other) { return 0; }
  virtual void SetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other, i32 reaction) {}
  virtual i32 GetCrimeGold(papyrus::ObjectRef faction) { return 0; }
  virtual void SetCrimeGold(papyrus::ObjectRef faction, i32 gold) {}
  virtual void ModCrimeGold(papyrus::ObjectRef faction, i32 delta) {}

  // Quests (new in-engine state system).
  virtual i32 GetStage(papyrus::ObjectRef quest) { return 0; }
  virtual void SetStage(papyrus::ObjectRef quest, i32 stage) {}
  virtual bool GetStageDone(papyrus::ObjectRef quest, i32 stage) { return false; }
  virtual bool IsRunning(papyrus::ObjectRef quest) { return false; }
  virtual void StartQuest(papyrus::ObjectRef quest) {}
  virtual void StopQuest(papyrus::ObjectRef quest) {}
  virtual void ResetQuest(papyrus::ObjectRef quest) {}
  virtual bool IsQuestActive(papyrus::ObjectRef quest) { return false; }
  virtual void SetQuestActive(papyrus::ObjectRef quest, bool active) {}
  virtual void SetObjectiveDisplayed(papyrus::ObjectRef quest, i32 objective, bool displayed) {}
  virtual void SetObjectiveCompleted(papyrus::ObjectRef quest, i32 objective, bool completed) {}
  virtual bool IsObjectiveDisplayed(papyrus::ObjectRef quest, i32 objective) { return false; }
  virtual bool IsObjectiveCompleted(papyrus::ObjectRef quest, i32 objective) { return false; }

  // Scenes: a SCEN fragment runs Self.GetOwningQuest().SetStage(N), so the scene
  // object must resolve to its owning quest. Returns None for an unknown scene.
  virtual papyrus::ObjectRef SceneOwningQuest(papyrus::ObjectRef scene) { return papyrus::ObjectRef{0}; }
  // Scene.Start/ForceStart begin playing a scene (a quest fragment calls this);
  // Stop ends it; IsPlaying queries it. Default no-ops for the neutral bindings.
  virtual void SceneStart(papyrus::ObjectRef scene) {}
  virtual void SceneStop(papyrus::ObjectRef scene) {}
  virtual bool SceneIsPlaying(papyrus::ObjectRef scene) { return false; }

  // Player control gate (new system). Categories: 0 movement, 1 fighting,
  // 2 cam-switch, 3 looking, 4 sneaking, 5 menu, 6 activate, 7 journal,
  // 8 fast-travel. All enabled by default; cutscene scripts toggle them.
  static constexpr i32 kControlCount = 9;
  virtual void SetPlayerControl(i32 category, bool enabled) {}
  virtual bool IsPlayerControlEnabled(i32 category) { return true; }

  // Global variables: initial value from the GLOB record, overridable.
  virtual f32 GetGlobalValue(papyrus::ObjectRef global) { return 0; }
  virtual void SetGlobalValue(papyrus::ObjectRef global, f32 value) {}

  // Game settings and time.
  virtual f32 GetGameSettingFloat(const std::string& name) { return 0; }
  virtual f32 GetRealHoursPassed() { return 0; }
  virtual f32 GetCurrentGameTime() { return 0; }  // days since game start
};

// Registers the Skyrim Papyrus native surface into reg, bound against bindings
// (pass nullptr to use neutral defaults). Math/Utility globals are fully
// implemented here; engine-touching natives route through bindings.
void RegisterSkyrimNatives(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_
