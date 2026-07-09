// scenetest: deterministic checks for the engine-native scene runner. The
// runner holds no engine state, so a recording sink scripts every query and
// records every side effect. No game data needed, so it runs in the ctest gate.

#include <cstdio>
#include <vector>

#include "core/types.h"
#include "quest/scene.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Records every side effect and lets the test script the query answers:
// ActorAt/PlayerNear return false until their poll counter reaches a threshold.
struct MockSink : SceneSink {
  std::vector<u64> guided;       // actors handed a GuideTo target
  std::vector<u64> said;         // INFO handles run via SayInfo
  std::vector<std::pair<u64, i32>> staged;  // (quest, stage) from SetStage

  int actor_at_polls = 0;        // ActorAt calls so far
  int actor_at_true_after = 0;   // ActorAt returns true once polls > this
  int player_near_polls = 0;
  int player_near_true_after = 0;

  void GuideTo(u64 actor, const float[3]) override { guided.push_back(actor); }
  void SayInfo(u64 actor, u64 info) override {
    said.push_back(info);
    (void)actor;
  }
  void SetStage(u64 quest, i32 stage) override { staged.emplace_back(quest, stage); }
  bool ActorAt(u64, const float[3], float) override {
    return ++actor_at_polls > actor_at_true_after;
  }
  bool PlayerNear(const float[3], float) override {
    return ++player_near_polls > player_near_true_after;
  }
};

SceneAction GuideTo(u64 actor, float x, float y, float z, float radius = 2.5f) {
  SceneAction a;
  a.kind = SceneAction::Kind::kGuideTo;
  a.actor = actor;
  a.pos[0] = x;
  a.pos[1] = y;
  a.pos[2] = z;
  a.radius = radius;
  return a;
}

SceneAction SetStage(u64 quest, i32 stage) {
  SceneAction a;
  a.kind = SceneAction::Kind::kSetStage;
  a.quest = quest;
  a.stage = stage;
  return a;
}

SceneAction SayInfo(u64 actor, u64 info) {
  SceneAction a;
  a.kind = SceneAction::Kind::kSayInfo;
  a.actor = actor;
  a.info = info;
  return a;
}

SceneAction Wait(float seconds) {
  SceneAction a;
  a.kind = SceneAction::Kind::kWait;
  a.seconds = seconds;
  return a;
}

SceneAction WaitPlayerNear(float x, float y, float z, float radius = 2.5f) {
  SceneAction a;
  a.kind = SceneAction::Kind::kWaitPlayerNear;
  a.pos[0] = x;
  a.pos[1] = y;
  a.pos[2] = z;
  a.radius = radius;
  return a;
}

void TestSetStage() {
  std::puts("scene set stage:");
  Scene scene;
  scene.actions.push_back(SetStage(0xABCD, 10));

  MockSink sink;
  SceneRunner runner(&scene);
  Check("running before first tick", runner.running());

  // A SetStage action completes immediately, so this single tick advances past
  // the only action and reports the scene finished.
  bool still = runner.Tick(sink, 0.016f);
  Check("scene finished after one tick", !still);
  Check("not running after finish", !runner.running());
  Check("SetStage called exactly once", sink.staged.size() == 1);
  Check("SetStage carries quest + stage",
        sink.staged.size() == 1 && sink.staged[0].first == 0xABCD &&
            sink.staged[0].second == 10);

  // Ticking a finished runner stays finished and issues nothing more.
  Check("tick past end stays finished", !runner.Tick(sink, 0.016f));
  Check("no extra SetStage after finish", sink.staged.size() == 1);
}

void TestGuideTo() {
  std::puts("scene guide to:");
  Scene scene;
  scene.actions.push_back(GuideTo(7, 1, 2, 3));

  MockSink sink;
  sink.actor_at_true_after = 3;  // arrives only on the 4th poll
  SceneRunner runner(&scene);

  Check("first tick issues GuideTo", runner.Tick(sink, 0.1f) && sink.guided.size() == 1);
  Check("GuideTo targets the actor", sink.guided[0] == 7);
  Check("still running while not arrived", runner.running());

  // Holds at the guide action while ActorAt is false, never re-issuing GuideTo.
  runner.Tick(sink, 0.1f);
  runner.Tick(sink, 0.1f);
  Check("does not advance before arrival", runner.current_action() == 0);
  Check("GuideTo issued only once", sink.guided.size() == 1);

  // The 4th poll returns true, so this tick advances past the lone action.
  bool still = runner.Tick(sink, 0.1f);
  Check("advances on arrival", !still && !runner.running());
  Check("GuideTo not re-issued on arrival", sink.guided.size() == 1);
}

void TestWait() {
  std::puts("scene wait:");
  Scene scene;
  scene.actions.push_back(Wait(0.5f));

  MockSink sink;
  SceneRunner runner(&scene);

  // 0.2s + 0.2s = 0.4s is short of 0.5s, so the runner holds.
  Check("running after 0.2s", runner.Tick(sink, 0.2f));
  Check("running after 0.4s", runner.Tick(sink, 0.2f));
  Check("still on the wait action", runner.current_action() == 0);

  // Crossing 0.5s completes the wait, finishing the scene.
  Check("finished once duration elapsed", !runner.Tick(sink, 0.2f));
  Check("not running after wait", !runner.running());
}

void TestWaitPlayerNear() {
  std::puts("scene wait player near:");
  Scene scene;
  scene.actions.push_back(WaitPlayerNear(10, 0, 10));

  MockSink sink;
  sink.player_near_true_after = 2;  // near only on the 3rd poll
  SceneRunner runner(&scene);

  Check("holds while player is far (1)", runner.Tick(sink, 0.1f));
  Check("holds while player is far (2)", runner.Tick(sink, 0.1f));
  Check("still waiting on player", runner.current_action() == 0);
  Check("finishes when player arrives", !runner.Tick(sink, 0.1f));
  Check("not running after player arrives", !runner.running());
}

void TestFullScene() {
  std::puts("scene full run:");
  Scene scene;
  scene.actions.push_back(GuideTo(1, 0, 0, 0));
  scene.actions.push_back(SetStage(0x100, 10));
  scene.actions.push_back(GuideTo(2, 5, 0, 5));
  scene.actions.push_back(SetStage(0x100, 20));

  MockSink sink;  // ActorAt true on its first poll, so each guide arrives at once
  SceneRunner runner(&scene);

  int guard = 0;
  while (runner.Tick(sink, 0.1f) && guard < 100) ++guard;
  Check("scene ran to completion", !runner.running());
  Check("did not spin", guard < 100);

  Check("both guides issued", sink.guided.size() == 2 && sink.guided[0] == 1 &&
                                  sink.guided[1] == 2);
  Check("two stages set in order",
        sink.staged.size() == 2 && sink.staged[0] == std::make_pair<u64, i32>(0x100, 10) &&
            sink.staged[1] == std::make_pair<u64, i32>(0x100, 20));

  // A finished runner keeps reporting finished.
  Check("tick after completion returns false", !runner.Tick(sink, 0.1f));
}

void TestSayInfo() {
  std::puts("scene say info:");
  Scene scene;
  scene.actions.push_back(SayInfo(9, 0xBEEF));

  MockSink sink;
  SceneRunner runner(&scene);
  Check("SayInfo completes immediately", !runner.Tick(sink, 0.016f));
  Check("SayInfo called once with info handle",
        sink.said.size() == 1 && sink.said[0] == 0xBEEF);
}

void TestReset() {
  std::puts("scene reset:");
  Scene scene;
  scene.actions.push_back(SetStage(0x1, 1));
  scene.actions.push_back(SetStage(0x1, 2));

  MockSink sink;
  SceneRunner runner;
  Check("no scene means not running", !runner.running());
  Check("ticking with no scene returns false", !runner.Tick(sink, 0.1f));

  runner.Reset(&scene);
  Check("running after reset", runner.running());
  Check("reset starts at action 0", runner.current_action() == 0);
  runner.Tick(sink, 0.1f);  // sets stage 1, advances to action 1
  Check("advanced to action 1", runner.current_action() == 1);
  Check("one stage set so far", sink.staged.size() == 1);

  // Reset rewinds to action 0 so the scene replays from the top: both stages
  // run again, on top of the single stage set before the reset.
  runner.Reset(&scene);
  Check("reset rewinds to action 0", runner.current_action() == 0);
  Check("running again after reset", runner.running());
  while (runner.Tick(sink, 0.1f)) {}
  Check("replay re-ran both stages", sink.staged.size() == 3);
  Check("replay set stages 1 then 2",
        sink.staged[1] == std::make_pair<u64, i32>(0x1, 1) &&
            sink.staged[2] == std::make_pair<u64, i32>(0x1, 2));
}

}  // namespace

int main() {
  TestSetStage();
  TestGuideTo();
  TestWait();
  TestWaitPlayerNear();
  TestSayInfo();
  TestFullScene();
  TestReset();
  if (g_failures == 0) {
    std::puts("scene: all checks passed");
    return 0;
  }
  std::printf("scene: %d checks FAILED\n", g_failures);
  return 1;
}
