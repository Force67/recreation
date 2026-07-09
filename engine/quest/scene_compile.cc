#include "quest/scene_compile.h"

namespace rx::quest {

Scene CompileScene(const SceneDef& def, const SceneBindings& bindings) {
  Scene out;
  auto resolve_actor = [&](i32 alias) -> u64 {
    return bindings.actor ? bindings.actor(alias) : 0;
  };

  // Emit one action's beats; shared by the phase-ordered and the no-phase paths.
  auto emit = [&](const SceneActionDef& a) {
    switch (a.kind) {
      case SceneActionDef::Kind::kDialogue: {
        const u64 actor = resolve_actor(a.actor_alias);
        if (!actor) return;
        const u64 info = bindings.info ? bindings.info(a.topic, actor) : 0;
        if (!info) return;  // the speaker has no line under this topic
        SceneAction sa;
        sa.kind = SceneAction::Kind::kSayInfo;
        sa.actor = actor;
        sa.info = info;
        out.actions.push_back(sa);
        break;
      }
      case SceneActionDef::Kind::kPackage: {
        const u64 actor = resolve_actor(a.actor_alias);
        if (!actor) return;
        f32 pos[3] = {0, 0, 0};
        f32 radius = 2.5f;
        if (!bindings.package_target || !bindings.package_target(a.package, pos, &radius)) return;
        SceneAction sa;
        sa.kind = SceneAction::Kind::kGuideTo;
        sa.actor = actor;
        sa.pos[0] = pos[0];
        sa.pos[1] = pos[1];
        sa.pos[2] = pos[2];
        sa.radius = radius;
        out.actions.push_back(sa);
        break;
      }
      case SceneActionDef::Kind::kTimer: {
        SceneAction sa;
        sa.kind = SceneAction::Kind::kWait;
        sa.seconds = a.timer_seconds;
        out.actions.push_back(sa);
        break;
      }
      case SceneActionDef::Kind::kUnknown:
        break;
    }
  };

  if (def.phases.empty()) {
    for (const SceneActionDef& a : def.actions) emit(a);
    return out;
  }
  // Phases run in declaration order; within a phase, the actions that begin in
  // it, in record order.
  for (const ScenePhaseDef& phase : def.phases)
    for (const SceneActionDef& a : def.actions)
      if (a.start_phase == phase.index) emit(a);
  return out;
}

}  // namespace rx::quest
