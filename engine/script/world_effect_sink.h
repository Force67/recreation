#ifndef RECREATION_SCRIPT_WORLD_EFFECT_SINK_H_
#define RECREATION_SCRIPT_WORLD_EFFECT_SINK_H_

#include "core/types.h"

namespace rec::script {

// Guest-thread-facing sink for quest-driven world mutations. The Skyrim bindings
// call these; the runtime implements them by marshaling to the main thread's ECS
// (QuestWorld), which records per-quest provenance so the effects can be rolled
// back wholesale. SpawnReference allocates and returns the new reference handle
// synchronously, so PlaceAtMe can hand it straight back to the script.
//
// `quest` is the quest a mutation is attributed to (0 = unattributed, not rolled
// back); the bindings pass the quest whose fragment is currently running.
class WorldEffectSink {
 public:
  virtual ~WorldEffectSink() = default;

  virtual u64 SpawnReference(u64 quest, u64 base, f32 x, f32 y, f32 z) = 0;
  virtual void MoveReference(u64 quest, u64 handle, f32 x, f32 y, f32 z) = 0;
  // `dest_ref` is the reference the player was moved to (0 for a raw SetPosition).
  // When it lives in an interior cell, the runtime streams that cell so a quest
  // that warps the player indoors (e.g. into the Helgen keep) lands them there.
  virtual void MovePlayer(u64 quest, u64 dest_ref, f32 x, f32 y, f32 z) = 0;
  virtual void SetEnabled(u64 quest, u64 handle, bool enabled) = 0;
  virtual void DeleteReference(u64 quest, u64 handle) = 0;
  virtual void CleanupQuest(u64 quest) = 0;

  // Combat enrollment, marshaled to the main-thread combat driver. StartCombat
  // makes `attacker` fight `target`; StopCombat withdraws it; ActorDied drops a
  // dead actor and anyone targeting it. Default no-ops so a sink that does not
  // simulate combat (tests, headless tools) need not implement them.
  virtual void StartCombat(u64 quest, u64 attacker, u64 target) {}
  virtual void StopCombat(u64 quest, u64 attacker) {}
  virtual void ActorDied(u64 quest, u64 actor) {}
  // An actor was reset/resurrected by a script (Reset, ResurrectActor): clear the
  // downed state so it renders and fights again. Mirrors ActorDied.
  virtual void ActorResurrected(u64 quest, u64 actor) {}
};

}  // namespace rec::script

#endif  // RECREATION_SCRIPT_WORLD_EFFECT_SINK_H_
