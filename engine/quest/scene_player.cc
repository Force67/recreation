#include "quest/scene_player.h"

#include <utility>

namespace rx::quest {

// Every sink call can re-enter the player: a fragment it fires may SetStage,
// whose stage fragment Starts/Stops another scene, mutating active_. So we never
// hold a map iterator/reference across a sink call -- always re-find by handle
// afterwards and bail if the scene is gone.

void ScenePlayer::Start(u64 scene, std::vector<u32> phases, f32 phase_seconds,
                        ScenePlayerSink& sink) {
  Active a;
  a.phases = std::move(phases);
  a.period = phase_seconds > 0 ? phase_seconds : 1.0f;
  const bool has_phases = !a.phases.empty();
  const u32 first = has_phases ? a.phases[0] : 0;
  active_[scene] = std::move(a);

  sink.SceneBegin(scene);  // may re-enter
  if (active_.find(scene) == active_.end()) return;  // a re-entrant Stop removed it
  if (!has_phases) {
    active_.erase(scene);  // nothing to play through; begin and end at once
    sink.SceneEnd(scene);
    return;
  }
  sink.ScenePhase(scene, first, true);
}

void ScenePlayer::Stop(u64 scene, ScenePlayerSink& sink) {
  auto it = active_.find(scene);
  if (it == active_.end()) return;
  Active a = std::move(it->second);  // copy out, then remove before firing cues
  active_.erase(it);
  if (a.current < a.phases.size()) sink.ScenePhase(scene, a.phases[a.current], false);
  sink.SceneEnd(scene);
}

void ScenePlayer::Tick(f32 dt, ScenePlayerSink& sink) {
  // Snapshot the handles: the sink can add/remove entries re-entrantly.
  std::vector<u64> handles;
  handles.reserve(active_.size());
  for (const auto& [scene, a] : active_) handles.push_back(scene);

  for (u64 scene : handles) {
    if (auto it = active_.find(scene); it != active_.end())
      it->second.timer += dt;  // accumulate once per tick
    // Fire as many phase transitions as the timer allows, re-finding each step.
    for (;;) {
      auto it = active_.find(scene);
      if (it == active_.end()) break;
      Active& a = it->second;
      if (a.current >= a.phases.size() || a.timer < a.period) break;
      a.timer -= a.period;
      const u32 cur = a.phases[a.current];
      sink.ScenePhase(scene, cur, false);  // end the current phase (may re-enter)

      it = active_.find(scene);
      if (it == active_.end()) break;
      Active& a2 = it->second;
      ++a2.current;
      if (a2.current < a2.phases.size()) {
        sink.ScenePhase(scene, a2.phases[a2.current], true);  // begin the next
      } else {
        active_.erase(scene);
        sink.SceneEnd(scene);
        break;
      }
    }
  }
}

}  // namespace rx::quest
