// scene_playertest: checks the timer-driven ScenePlayer fires begin/phase/end
// cues in order. Deterministic (no game data), runs in the ctest gate.

#include <cstdio>
#include <string>
#include <vector>

#include "quest/scene_player.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Records the cue sequence as readable strings so order is easy to assert.
struct RecordingSink : ScenePlayerSink {
  std::vector<std::string> log;
  void SceneBegin(u64 scene) override { log.push_back("begin " + std::to_string(scene)); }
  void ScenePhase(u64 scene, u32 phase, bool on_begin) override {
    log.push_back("phase " + std::to_string(scene) + ":" + std::to_string(phase) +
                  (on_begin ? " begin" : " end"));
  }
  void SceneEnd(u64 scene) override { log.push_back("end " + std::to_string(scene)); }
};

void TestTwoPhases() {
  std::puts("scene player (two phases):");
  ScenePlayer player;
  RecordingSink s;
  player.Start(7, {5, 10}, 2.0f, s);
  Check("begin + first phase begin fire immediately",
        s.log.size() == 2 && s.log[0] == "begin 7" && s.log[1] == "phase 7:5 begin");
  Check("playing", player.IsPlaying(7));

  player.Tick(1.0f, s);  // under the period: nothing new
  Check("no cue before the period elapses", s.log.size() == 2);

  player.Tick(1.0f, s);  // reaches the period: phase 5 ends, phase 10 begins
  Check("phase 5 ends then phase 10 begins",
        s.log.size() == 4 && s.log[2] == "phase 7:5 end" && s.log[3] == "phase 7:10 begin");

  player.Tick(2.0f, s);  // last phase ends -> scene ends
  Check("phase 10 ends then scene ends",
        s.log.size() == 6 && s.log[4] == "phase 7:10 end" && s.log[5] == "end 7");
  Check("not playing after it finishes", !player.IsPlaying(7));

  player.Tick(5.0f, s);  // ticking a finished player does nothing
  Check("finished player is inert", s.log.size() == 6);
}

void TestNoPhases() {
  std::puts("scene player (no phases):");
  ScenePlayer player;
  RecordingSink s;
  player.Start(3, {}, 1.0f, s);
  Check("a phaseless scene begins and ends at once",
        s.log.size() == 2 && s.log[0] == "begin 3" && s.log[1] == "end 3");
  Check("not left playing", !player.IsPlaying(3));
}

void TestStopMidplay() {
  std::puts("scene player (stop mid-play):");
  ScenePlayer player;
  RecordingSink s;
  player.Start(9, {1, 2, 3}, 4.0f, s);  // begin, phase 1 begin
  player.Stop(9, s);
  Check("stop fires the current phase end then scene end",
        s.log.size() == 4 && s.log[2] == "phase 9:1 end" && s.log[3] == "end 9");
  Check("not playing after stop", !player.IsPlaying(9));
  player.Stop(9, s);  // stopping again is a no-op
  Check("double stop is inert", s.log.size() == 4);
}

void TestMultipleScenes() {
  std::puts("scene player (two scenes at once):");
  ScenePlayer player;
  RecordingSink s;
  player.Start(1, {0}, 1.0f, s);
  player.Start(2, {0}, 1.0f, s);
  Check("both playing", player.playing_count() == 2);
  player.Tick(1.0f, s);  // both single-phase scenes finish this tick
  Check("both scenes finished", player.playing_count() == 0);
}

}  // namespace

int main() {
  TestTwoPhases();
  TestNoPhases();
  TestStopMidplay();
  TestMultipleScenes();
  if (g_failures) {
    std::printf("scene player: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::puts("scene player: all checks passed");
  return 0;
}
