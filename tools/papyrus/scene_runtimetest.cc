// scene_runtimetest: the phase machine behind every cutscene. Checks that a
// lowered SCEN plays its phases in order, speaks one line at a time for as long
// as the line lasts, holds packages across their phase window, waits on a
// completion gate and gives up on one that never passes. Deterministic, no game
// data, runs in the ctest gate.

#include <cstdio>
#include <string>
#include <vector>

#include "quest/scene_runtime.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

struct RecordingSink : SceneRuntimeSink {
  std::vector<std::string> log;
  bool gate_passes = true;
  int gate_queries = 0;

  void OnSceneBegin(const ScenePlan&) override { log.push_back("scene begin"); }
  void OnSceneEnd(const ScenePlan&, bool completed) override {
    log.push_back(completed ? "scene end" : "scene stopped");
  }
  void OnPhaseBegin(const ScenePlan&, i32 phase) override {
    log.push_back("phase " + std::to_string(phase) + " begin");
  }
  void OnPhaseEnd(const ScenePlan&, i32 phase) override {
    log.push_back("phase " + std::to_string(phase) + " end");
  }
  void OnLineBegin(const ScenePlan&, const SceneBeat& b) override {
    log.push_back("say " + b.speaker + ": " + b.text);
  }
  void OnLineEnd(const ScenePlan&, const SceneBeat& b) override {
    log.push_back("said " + b.speaker);
  }
  void OnPackageBegin(const ScenePlan&, const SceneBeat& b) override {
    log.push_back("package " + std::to_string(b.package) + " on");
  }
  void OnPackageEnd(const ScenePlan&, const SceneBeat& b) override {
    log.push_back("package " + std::to_string(b.package) + " off");
  }
  bool ConditionsPass(const ConditionList&) override {
    ++gate_queries;
    return gate_passes;
  }
};

SceneBeat Line(i32 phase, const char* who, const char* text, f32 seconds) {
  SceneBeat b;
  b.kind = SceneBeat::Kind::kDialogue;
  b.phase = phase;
  b.end_phase = phase;
  b.actor = 0x100;
  b.speaker = who;
  b.text = text;
  b.seconds = seconds;
  return b;
}

// A three-phase scene: two lines in phase 0, one in phase 2, and a package that
// spans both, which is how a scene keeps an actor walking while people talk.
ScenePlan TwoPhasePlan() {
  ScenePlan plan;
  plan.scene = 0xbecd4;
  plan.quest = 0x3372b;
  plan.phases = {0, 1, 2};
  plan.completion = {ConditionList{}, ConditionList{}, ConditionList{}};
  plan.beats.push_back(Line(0, "Ralof", "Hey, you. You're finally awake.", 2.0f));
  plan.beats.push_back(Line(0, "Lokir", "Damn you Stormcloaks.", 1.0f));
  SceneBeat pack;
  pack.kind = SceneBeat::Kind::kPackage;
  pack.phase = 0;
  pack.end_phase = 1;
  pack.package = 42;
  pack.actor = 0x200;
  plan.beats.push_back(pack);
  plan.beats.push_back(Line(2, "Ralof", "Sovngarde awaits.", 1.0f));
  return plan;
}

void TestPhaseOrder() {
  std::puts("scene runtime (phases, lines, packages):");
  const ScenePlan plan = TwoPhasePlan();
  SceneRuntime run;
  RecordingSink s;
  run.Start(&plan, s);
  Check("begins with the scene, the first phase, its package and its first line",
        s.log.size() == 4 && s.log[0] == "scene begin" && s.log[1] == "phase 0 begin" &&
            s.log[2] == "package 42 on" && s.log[3] == "say Ralof: Hey, you. You're finally awake.");
  Check("the spoken line is queryable for the subtitle",
        run.speaking() != nullptr && run.speaking()->speaker == "Ralof");

  run.Tick(1.0f, s);
  Check("a line holds for its full length", s.log.size() == 4);
  run.Tick(1.1f, s);
  Check("the next speaker follows the first",
        s.log.size() == 6 && s.log[4] == "said Ralof" && s.log[5] == "say Lokir: Damn you Stormcloaks.");

  // Lokir finishing empties phase 0. Phase 1 holds nothing, so the same tick runs
  // through it into phase 2 rather than spending a frame per empty phase.
  run.Tick(1.1f, s);
  Check("an empty phase passes straight through",
        s.log.size() == 13 && s.log[6] == "said Lokir" && s.log[7] == "phase 0 end" &&
            s.log[8] == "phase 1 begin" && s.log[9] == "phase 1 end");
  Check("the package holds across its whole window, then stops as phase 2 opens",
        s.log[10] == "package 42 off" && s.log[11] == "phase 2 begin");
  Check("the last phase speaks its line",
        s.log[12] == "say Ralof: Sovngarde awaits." && run.speaking() != nullptr &&
            run.phase() == 2);
  run.Tick(1.2f, s);
  Check("the scene ends after the last phase", !run.playing() && s.log.back() == "scene end");
}

void TestCompletionGate() {
  std::puts("scene runtime (completion gate):");
  ScenePlan plan;
  plan.phases = {0, 1};
  ConditionList gate;
  Comparison c;
  c.func = Func::kGetStage;
  c.param1 = 0x3372b;
  c.op = CompareOp::kGreaterOrEqual;
  c.value = 20;
  gate.comparisons.push_back(c);
  plan.completion = {gate, ConditionList{}};
  plan.beats.push_back(Line(0, "Ralof", "wait for it", 0.5f));
  plan.beats.push_back(Line(1, "Ralof", "there it is", 0.5f));

  SceneRuntime run;
  run.set_phase_timeout(4.0f);
  RecordingSink s;
  s.gate_passes = false;
  run.Start(&plan, s);
  run.Tick(0.6f, s);  // the line is done, the gate now holds the phase
  Check("a phase whose gate fails does not advance", run.phase() == 0 && s.gate_queries > 0);
  s.gate_passes = true;
  run.Tick(0.1f, s);
  Check("it advances the frame the gate passes", run.phase() == 1);

  // The same scene with a gate that never passes must still finish, on the timeout.
  SceneRuntime stuck;
  stuck.set_phase_timeout(2.0f);
  RecordingSink s2;
  s2.gate_passes = false;
  stuck.Start(&plan, s2);
  for (int i = 0; i < 40 && stuck.playing(); ++i) stuck.Tick(0.2f, s2);
  Check("a gate that never passes gives way to the timeout", !stuck.playing());
}

void TestStopAndBuild() {
  std::puts("scene runtime (stop, and lowering a parsed scene):");
  const ScenePlan plan = TwoPhasePlan();
  SceneRuntime run;
  RecordingSink s;
  run.Start(&plan, s);
  run.Stop(s);
  Check("stop closes the line, the phase, the package and the scene",
        !run.playing() && s.log[s.log.size() - 4] == "said Ralof" &&
            s.log[s.log.size() - 3] == "phase 0 end" &&
            s.log[s.log.size() - 2] == "package 42 off" && s.log.back() == "scene stopped");

  // Lowering: two dialogue actions and one package, phases out of order in the
  // record, one action naming a phase the phase list never declared.
  SceneDef def;
  def.handle = 7;
  def.quest = 9;
  def.phases.push_back({2, {}, {}});
  def.phases.push_back({0, {}, {}});
  SceneActionDef d0;
  d0.kind = SceneActionDef::Kind::kDialogue;
  d0.actor_alias = 3;
  d0.topic = 0xaaa;
  d0.start_phase = 0;
  d0.end_phase = 0;
  def.actions.push_back(d0);
  SceneActionDef d1 = d0;
  d1.start_phase = 5;  // past the last declared phase
  d1.end_phase = 5;
  d1.topic = 0xbbb;
  def.actions.push_back(d1);
  SceneActionDef pk;
  pk.kind = SceneActionDef::Kind::kPackage;
  pk.actor_alias = 4;
  pk.package = 0xcc;
  pk.start_phase = 0;
  pk.end_phase = 2;
  def.actions.push_back(pk);

  ScenePlanBindings b;
  b.actor = [](i32 alias) { return static_cast<u64>(0x1000 + alias); };
  b.alias_name = [](i32 alias) { return alias == 3 ? std::string("Ralof") : std::string("Horse"); };
  b.line = [](i32 alias, u64 topic, u64 speaker, u64* info, std::string* text, f32* seconds) {
    if (speaker == 0 || alias < 0) return false;
    *info = topic | 0xf0000;
    *text = topic == 0xaaa ? "first" : "last";
    *seconds = 3.0f;
    return true;
  };
  const ScenePlan built = BuildScenePlan(def, b);
  Check("phases are sorted and an undeclared action phase is added",
        built.phases.size() == 3 && built.phases[0] == 0 && built.phases[1] == 2 &&
            built.phases[2] == 5);
  Check("beats carry resolved actors, text and length",
        built.beats.size() == 3 && built.beats[0].text == "first" &&
            built.beats[0].actor == 0x1003 && built.beats[0].speaker == "Ralof" &&
            built.beats[0].seconds >= 3.0f);
  Check("the package keeps its window", built.beats[1].kind == SceneBeat::Kind::kPackage &&
                                           built.beats[1].phase == 0 &&
                                           built.beats[1].end_phase == 2);
  Check("beats are in play order", built.beats[2].phase == 5 && built.beats[2].text == "last");
}

}  // namespace

int main() {
  TestPhaseOrder();
  TestCompletionGate();
  TestStopAndBuild();
  if (g_failures) {
    std::printf("scene runtime: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::puts("scene runtime: all checks passed");
  return 0;
}
