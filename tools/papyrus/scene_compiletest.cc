#include <cstdio>

#include "quest/scene_compile.h"

using namespace rx;
using namespace rx::quest;

static int g_failures = 0;
static void Check(const char* what, bool ok) {
  if (!ok) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  } else {
    std::printf("  [ok] %s\n", what);
  }
}

int main() {
  std::printf("scene compile:\n");

  // A two-phase scene: phase 0 has a dialogue beat by alias 5 and a timer; phase
  // 1 has a travel package by alias 6. Actions are listed out of phase order to
  // prove the compiler emits them in phase order.
  SceneDef def;
  def.handle = 0xabc;
  def.quest = 0x3372b;
  def.phases.push_back({0, {}, {}});
  def.phases.push_back({1, {}, {}});

  SceneActionDef pkg;
  pkg.kind = SceneActionDef::Kind::kPackage;
  pkg.actor_alias = 6;
  pkg.start_phase = 1;
  pkg.package = 0xBEEF;
  def.actions.push_back(pkg);

  SceneActionDef talk;
  talk.kind = SceneActionDef::Kind::kDialogue;
  talk.actor_alias = 5;
  talk.start_phase = 0;
  talk.topic = 0xD1A1;
  def.actions.push_back(talk);

  SceneActionDef timer;
  timer.kind = SceneActionDef::Kind::kTimer;
  timer.start_phase = 0;
  timer.timer_seconds = 2.5f;
  def.actions.push_back(timer);

  SceneActionDef unfilled;  // alias 9 is not bound -> skipped
  unfilled.kind = SceneActionDef::Kind::kDialogue;
  unfilled.actor_alias = 9;
  unfilled.start_phase = 0;
  unfilled.topic = 0xD1A2;
  def.actions.push_back(unfilled);

  SceneBindings b;
  b.actor = [](i32 alias) -> u64 {
    if (alias == 5) return 0xAC705;
    if (alias == 6) return 0xAC706;
    return 0;  // alias 9 unfilled
  };
  b.info = [](u64 topic, u64 speaker) -> u64 {
    if (topic == 0xD1A1 && speaker == 0xAC705) return 0x1F0;
    return 0;
  };
  b.package_target = [](u64 package, f32 pos[3], f32* radius) {
    if (package != 0xBEEF) return false;
    pos[0] = 10.0f;
    pos[1] = 1.0f;
    pos[2] = -4.0f;
    *radius = 3.0f;
    return true;
  };

  Scene scene = CompileScene(def, b);

  // Expected emission order: phase 0 dialogue (alias 5), phase 0 timer, phase 1
  // package (alias 6). The unfilled alias-9 dialogue is dropped.
  Check("three beats compiled", scene.actions.size() == 3);
  if (scene.actions.size() == 3) {
    const SceneAction& a0 = scene.actions[0];
    Check("beat 0 is dialogue", a0.kind == SceneAction::Kind::kSayInfo);
    Check("beat 0 actor resolved", a0.actor == 0xAC705);
    Check("beat 0 info resolved", a0.info == 0x1F0);
    const SceneAction& a1 = scene.actions[1];
    Check("beat 1 is wait", a1.kind == SceneAction::Kind::kWait);
    Check("beat 1 seconds", a1.seconds == 2.5f);
    const SceneAction& a2 = scene.actions[2];
    Check("beat 2 is guide (package target)", a2.kind == SceneAction::Kind::kGuideTo);
    Check("beat 2 actor resolved", a2.actor == 0xAC706);
    Check("beat 2 target x", a2.pos[0] == 10.0f);
    Check("beat 2 radius", a2.radius == 3.0f);
  }

  // No bindings -> dialogue/package drop (unresolvable), but a timer beat does
  // not depend on bindings, so only the kWait survives. No crash.
  Scene unbound = CompileScene(def, SceneBindings{});
  Check("unbound scene keeps only the timer beat",
        unbound.actions.size() == 1 && unbound.actions[0].kind == SceneAction::Kind::kWait);

  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
