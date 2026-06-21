#include "quest/scene_player.h"

#include <utility>

namespace rec::quest {

void ScenePlayer::Start(u64 scene, std::vector<u32> phases, f32 phase_seconds,
                        ScenePlayerSink& sink) {
  Active a;
  a.phases = std::move(phases);
  a.period = phase_seconds > 0 ? phase_seconds : 1.0f;
  active_[scene] = std::move(a);

  sink.SceneBegin(scene);
  const std::vector<u32>& live = active_[scene].phases;
  if (live.empty()) {
    active_.erase(scene);  // nothing to play through; begin and end at once
    sink.SceneEnd(scene);
    return;
  }
  sink.ScenePhase(scene, live[0], true);
}

void ScenePlayer::Stop(u64 scene, ScenePlayerSink& sink) {
  auto it = active_.find(scene);
  if (it == active_.end()) return;
  Active a = std::move(it->second);
  active_.erase(it);
  if (a.current < a.phases.size()) sink.ScenePhase(scene, a.phases[a.current], false);
  sink.SceneEnd(scene);
}

void ScenePlayer::Tick(f32 dt, ScenePlayerSink& sink) {
  // SceneEnd is deferred until after the loop: erasing mid-iteration would
  // invalidate the map iterator, and a sink may itself start another scene.
  std::vector<u64> finished;
  for (auto& [scene, a] : active_) {
    a.timer += dt;
    while (a.timer >= a.period && a.current < a.phases.size()) {
      a.timer -= a.period;
      sink.ScenePhase(scene, a.phases[a.current], false);  // end the current phase
      ++a.current;
      if (a.current < a.phases.size())
        sink.ScenePhase(scene, a.phases[a.current], true);  // begin the next
      else
        finished.push_back(scene);
    }
  }
  for (u64 scene : finished) {
    active_.erase(scene);
    sink.SceneEnd(scene);
  }
}

}  // namespace rec::quest
