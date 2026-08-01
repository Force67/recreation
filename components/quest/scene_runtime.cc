#include "components/quest/scene_runtime.h"

#include <base/algorithm.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <algorithm>

namespace rx::quest {

ScenePlan BuildScenePlan(const SceneDef& def, const ScenePlanBindings& bindings) {
  ScenePlan plan;
  plan.scene = def.handle;
  plan.quest = def.quest;
  plan.flags = def.flags;

  for (const ScenePhaseDef& p : def.phases) {
    plan.phases.push_back(p.index);
    plan.completion.push_back(bindings.conditions ? bindings.conditions(p.completion)
                                                  : ConditionList{});
  }
  // A scene's actions can name phases the phase list does not declare (an action
  // window that runs past the last HNAM); play those too rather than dropping the
  // beat, otherwise the tail of a scene silently disappears.
  for (const SceneActionDef& a : def.actions) {
    if (std::find(plan.phases.begin(), plan.phases.end(), a.start_phase) != plan.phases.end())
      continue;
    plan.phases.push_back(a.start_phase);
    plan.completion.push_back(ConditionList{});
  }
  // Sort phases ascending, keeping each phase's completion gate with it.
  base::Vector<size_t> order(plan.phases.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return plan.phases[a] < plan.phases[b]; });
  base::Vector<i32> phases;
  base::Vector<ConditionList> completion;
  for (size_t i : order) {
    phases.push_back(plan.phases[i]);
    completion.push_back(plan.completion[i]);
  }
  plan.phases = base::move(phases);
  plan.completion = base::move(completion);

  auto name_of = [&](i32 alias) {
    return bindings.alias_name ? bindings.alias_name(alias) : base::String();
  };

  for (const SceneActionDef& a : def.actions) {
    SceneBeat beat;
    beat.phase = a.start_phase;
    beat.end_phase = base::Max(a.end_phase, a.start_phase);
    beat.alias = a.actor_alias;
    beat.actor = bindings.actor ? bindings.actor(a.actor_alias) : 0;
    beat.speaker = name_of(a.actor_alias);
    beat.look_at_alias = a.head_track_alias;
    switch (a.kind) {
      case SceneActionDef::Kind::kDialogue: {
        beat.kind = SceneBeat::Kind::kDialogue;
        beat.seconds = kSceneDefaultLineSeconds;
        if (bindings.line) {
          u64 info = 0;
          base::String text;
          f32 seconds = beat.seconds;
          if (bindings.line(a.actor_alias, a.topic, beat.actor, &info, &text, &seconds)) {
            beat.info = info;
            beat.text = base::move(text);
            beat.seconds = seconds;
          }
        }
        // A line with a delay authored on it (DMIN/DMAX) holds the phase that
        // much longer before the next speaker starts.
        beat.seconds += base::Max(a.delay_min, 0.0f);
        break;
      }
      case SceneActionDef::Kind::kPackage:
        beat.kind = SceneBeat::Kind::kPackage;
        beat.package = a.package;
        if (beat.package == 0) continue;
        break;
      case SceneActionDef::Kind::kTimer:
        beat.kind = SceneBeat::Kind::kTimer;
        beat.seconds = a.timer_seconds;
        break;
      case SceneActionDef::Kind::kUnknown:
        continue;
    }
    plan.beats.push_back(base::move(beat));
  }

  // Play order: by phase, then record order inside the phase.
  std::stable_sort(plan.beats.begin(), plan.beats.end(),
                   [](const SceneBeat& a, const SceneBeat& b) { return a.phase < b.phase; });
  return plan;
}

void SceneRuntime::Start(const ScenePlan* plan, SceneRuntimeSink& sink) {
  if (plan_) Stop(sink);
  if (!plan) return;
  plan_ = plan;
  time_ = 0;
  phase_index_ = 0;
  line_beat_ = kNoBeat;
  line_time_ = 0;
  active_packages_.clear();
  sink.OnSceneBegin(*plan_);
  if (plan_->phases.empty()) {
    Finish(true, sink);
    return;
  }
  EnterPhase(0, sink);
}

void SceneRuntime::Stop(SceneRuntimeSink& sink) {
  if (!plan_) return;
  EndLine(sink);
  LeavePhase(sink);
  Finish(false, sink);
}

i32 SceneRuntime::phase() const {
  if (!plan_ || phase_index_ >= plan_->phases.size()) return -1;
  return plan_->phases[phase_index_];
}

const SceneBeat* SceneRuntime::speaking() const {
  if (!plan_ || line_beat_ == kNoBeat) return nullptr;
  return &plan_->beats[line_beat_];
}

void SceneRuntime::EnterPhase(size_t index, SceneRuntimeSink& sink) {
  phase_index_ = index;
  phase_time_ = 0;
  phase_wait_ = 0;
  line_queue_.clear();
  line_cursor_ = 0;
  const i32 phase = plan_->phases[index];
  // Packages the outgoing phase left behind stop before the new phase opens, so a
  // performer is never briefly running two packages at once.
  ClosePackages(phase, sink);
  sink.OnPhaseBegin(*plan_, phase);
  for (size_t i = 0; i < plan_->beats.size(); ++i) {
    const SceneBeat& beat = plan_->beats[i];
    if (beat.phase != phase) continue;
    if (beat.kind == SceneBeat::Kind::kDialogue) line_queue_.push_back(i);
    if (beat.kind == SceneBeat::Kind::kTimer) phase_wait_ = base::Max(phase_wait_, beat.seconds);
  }
  OpenPackages(phase, sink);
  StartNextLine(sink);
}

void SceneRuntime::LeavePhase(SceneRuntimeSink& sink) {
  if (phase_index_ < plan_->phases.size()) sink.OnPhaseEnd(*plan_, plan_->phases[phase_index_]);
}

void SceneRuntime::ClosePackages(i32 phase, SceneRuntimeSink& sink) {
  for (size_t i = 0; i < active_packages_.size();) {
    const SceneBeat& beat = plan_->beats[active_packages_[i]];
    if (phase < beat.phase || phase > beat.end_phase) {
      sink.OnPackageEnd(*plan_, beat);
      active_packages_.erase(active_packages_.begin() + static_cast<ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
}

void SceneRuntime::OpenPackages(i32 phase, SceneRuntimeSink& sink) {
  for (size_t i = 0; i < plan_->beats.size(); ++i) {
    const SceneBeat& beat = plan_->beats[i];
    if (beat.kind != SceneBeat::Kind::kPackage) continue;
    if (phase < beat.phase || phase > beat.end_phase) continue;
    if (std::find(active_packages_.begin(), active_packages_.end(), i) != active_packages_.end())
      continue;
    active_packages_.push_back(i);
    sink.OnPackageBegin(*plan_, beat);
  }
}

void SceneRuntime::StartNextLine(SceneRuntimeSink& sink) {
  line_beat_ = kNoBeat;
  line_time_ = 0;
  if (line_cursor_ >= line_queue_.size()) return;
  line_beat_ = line_queue_[line_cursor_++];
  sink.OnLineBegin(*plan_, plan_->beats[line_beat_]);
}

void SceneRuntime::EndLine(SceneRuntimeSink& sink) {
  if (line_beat_ == kNoBeat) return;
  sink.OnLineEnd(*plan_, plan_->beats[line_beat_]);
  line_beat_ = kNoBeat;
  line_time_ = 0;
}

void SceneRuntime::Finish(bool completed, SceneRuntimeSink& sink) {
  const ScenePlan* plan = plan_;
  for (size_t i : active_packages_) sink.OnPackageEnd(*plan, plan->beats[i]);
  active_packages_.clear();
  plan_ = nullptr;
  line_queue_.clear();
  line_beat_ = kNoBeat;
  sink.OnSceneEnd(*plan, completed);
}

void SceneRuntime::Tick(f32 dt, SceneRuntimeSink& sink) {
  if (!plan_) return;
  time_ += dt;
  phase_time_ += dt;
  if (line_beat_ != kNoBeat) line_time_ += dt;
  if (phase_wait_ > 0) phase_wait_ -= dt;

  // Advance as far as this tick allows: a line that just finished hands straight
  // over to the next speaker, and a phase with nothing left in it to the next
  // phase, so a scene full of short empty phases does not burn a frame on each.
  static const ConditionList kNoGate;
  for (int guard = 0; plan_ != nullptr && guard < 64; ++guard) {
    if (line_beat_ != kNoBeat) {
      const SceneBeat& beat = plan_->beats[line_beat_];
      // Follow the voice clip while it is audible; `seconds` is the fallback for a
      // line with no clip, and the floor, so a failed play still holds the beat.
      if (sink.LineStillPlaying(beat) || line_time_ < beat.seconds) return;
      EndLine(sink);
      StartNextLine(sink);
      continue;
    }
    if (phase_wait_ > 0) return;

    // Everything authored in this phase has played. Its completion gate now decides
    // whether the scene moves on; a gate that never passes gives way to the timeout
    // so a cutscene cannot wedge on state the engine has no way to reach.
    const ConditionList& gate =
        phase_index_ < plan_->completion.size() ? plan_->completion[phase_index_] : kNoGate;
    if (!gate.empty() && !sink.ConditionsPass(gate) &&
        (phase_timeout_ <= 0 || phase_time_ < phase_timeout_))
      return;

    LeavePhase(sink);
    const size_t next = phase_index_ + 1;
    if (next >= plan_->phases.size()) {
      Finish(true, sink);
      return;
    }
    EnterPhase(next, sink);
  }
}

}  // namespace rx::quest
