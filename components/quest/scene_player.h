#ifndef RECREATION_QUEST_SCENE_PLAYER_H_
#define RECREATION_QUEST_SCENE_PLAYER_H_

#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace rx::quest {

// Receives a playing scene's fragment cues. The engine backs this with the
// bindings' RunSceneBegin/RunScenePhase/RunSceneEnd, which run the SCEN's
// Papyrus fragments; those fragments call Quest.SetStage to advance the journal.
class ScenePlayerSink {
 public:
  virtual ~ScenePlayerSink() = default;
  virtual void SceneBegin(u64 scene) = 0;
  virtual void ScenePhase(u64 scene, u32 phase, bool on_begin) = 0;
  virtual void SceneEnd(u64 scene) = 0;
};

// Plays started scenes through their phases on a fixed per-phase timer, firing
// the begin/phase/end cues in order. It is the engine's stand-in for the game's
// scene phase advancement (really driven by dialogue length and per-phase
// completion conditions); a fixed cadence keeps the quest progressing without
// that full runtime. Pure and unit-tested; the runtime owns one and ticks it.
class ScenePlayer {
 public:
  // Begins playing `scene`. `phases` is the ascending list of phase numbers that
  // carry a fragment; `phase_seconds` is how long each plays. Fires SceneBegin
  // and the first phase's begin immediately; a scene with no phases fires
  // SceneBegin then SceneEnd at once. Starting a playing scene restarts it.
  void Start(u64 scene, std::vector<u32> phases, f32 phase_seconds, ScenePlayerSink& sink);
  // Ends a scene now: fires the current phase's end (if mid-phase) then SceneEnd.
  // No-op if the scene is not playing.
  void Stop(u64 scene, ScenePlayerSink& sink);
  bool IsPlaying(u64 scene) const { return active_.count(scene) != 0; }
  size_t playing_count() const { return active_.size(); }
  // Advances every playing scene; fires each phase's end and the next phase's
  // begin as the timer elapses, and SceneEnd once the last phase finishes.
  void Tick(f32 dt, ScenePlayerSink& sink);

 private:
  struct Active {
    std::vector<u32> phases;
    size_t current = 0;  // index of the phase currently playing
    f32 timer = 0;       // seconds spent in the current phase
    f32 period = 1.0f;   // seconds per phase
  };
  std::unordered_map<u64, Active> active_;
};

}  // namespace rx::quest

#endif  // RECREATION_QUEST_SCENE_PLAYER_H_
