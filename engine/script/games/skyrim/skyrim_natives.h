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

  // Form
  virtual u32 GetFormId(papyrus::ObjectRef form) { return 0; }
  virtual i32 GetFormType(papyrus::ObjectRef form) { return 0; }
  virtual std::string GetName(papyrus::ObjectRef form) { return ""; }
  virtual bool HasKeyword(papyrus::ObjectRef form, papyrus::ObjectRef keyword) { return false; }

  // ObjectReference spatial state (engine transform / ECS).
  virtual f32 GetPositionX(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionY(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionZ(papyrus::ObjectRef ref) { return 0; }
  virtual void SetPosition(papyrus::ObjectRef ref, f32 x, f32 y, f32 z) {}
  virtual f32 GetDistance(papyrus::ObjectRef a, papyrus::ObjectRef b) { return 0; }
  virtual void MoveTo(papyrus::ObjectRef ref, papyrus::ObjectRef target) {}
  virtual void SetEnabled(papyrus::ObjectRef ref, bool enabled) {}
  virtual bool IsDisabled(papyrus::ObjectRef ref) { return false; }
  virtual i32 GetItemCount(papyrus::ObjectRef container, papyrus::ObjectRef item) { return 0; }
  virtual void AddItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) {}
  virtual void RemoveItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) {}
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

  // Actor values and state.
  virtual f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) { return 0; }
  virtual void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) {}
  virtual void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) {}
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
