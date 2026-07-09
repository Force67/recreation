// steering_avoidancetest: checks the context-steering obstacle picker (the
// follower "deflect around the wall" choice). Pure geometry, no game data.

#include <cmath>
#include <cstdio>

#include "world/steering_avoidance.h"

using rx::world::SteerAroundObstacles;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

int main() {
  const float goal[3] = {0, 0, 1};  // travel toward +z
  // Goal fanned dead-ahead, right, left (all unit XZ vectors).
  const float cands[9] = {
      0, 0, 1,   // +z, toward goal
      1, 0, 0,   // +x, right
      -1, 0, 0,  // -x, left
  };

  std::puts("context steering:");

  // All directions wide open: the candidate aligned with the goal wins.
  {
    const float clear[3] = {100, 100, 100};
    float out[3] = {0, 0, 0};
    SteerAroundObstacles(goal, cands, clear, 3, 50.0f, out);
    Check("all clear -> goal direction", Near(out[0], 0) && Near(out[2], 1) && Near(out[1], 0));
  }
  // Goal blocked by a wall, a side is open: deflect to the open side.
  {
    const float clear[3] = {0.1f, 100, 0.1f};  // +z and -x blocked, +x open
    float out[3] = {0, 0, 0};
    SteerAroundObstacles(goal, cands, clear, 3, 50.0f, out);
    Check("goal blocked -> deflect to open side", Near(out[0], 1) && Near(out[2], 0));
  }
  // Everything blocked: still returns a candidate, the most goal-aligned one.
  {
    const float clear[3] = {0.0f, 0.0f, 0.0f};
    float out[3] = {9, 9, 9};
    SteerAroundObstacles(goal, cands, clear, 3, 50.0f, out);
    const bool finite = std::isfinite(out[0]) && std::isfinite(out[2]);
    Check("all blocked -> no NaN, picks aligned goal",
          finite && Near(out[0], 0) && Near(out[2], 1));
  }
  // count == 0 copies goal_dir straight through.
  {
    float out[3] = {5, 5, 5};
    SteerAroundObstacles(goal, cands, nullptr, 0, 50.0f, out);
    Check("count 0 -> passes goal through",
          Near(out[0], goal[0]) && Near(out[1], goal[1]) && Near(out[2], goal[2]));
  }
  // A partially-blocked goal scores between fully clear and fully blocked, so a
  // fully clear side that is less aligned can still win once the gap is large.
  {
    // +z is half-clear (alignment_remap 1.0 -> score ~0.5), +x is fully clear
    // (alignment_remap 0.5 -> score 0.5). Make +z slightly under half so +x wins.
    const float clear[3] = {20.0f, 100.0f, 0.1f};  // +z 0.4 clearance, +x clear
    float out[3] = {0, 0, 0};
    SteerAroundObstacles(goal, cands, clear, 3, 50.0f, out);
    Check("partial goal loses to clear side", Near(out[0], 1) && Near(out[2], 0));
  }
  // The same partial goal beats a fully blocked side (clearance term dominates).
  {
    const float clear[3] = {25.0f, 0.1f, 0.1f};  // only +z has any clearance
    float out[3] = {0, 0, 0};
    SteerAroundObstacles(goal, cands, clear, 3, 50.0f, out);
    Check("partial goal beats blocked sides", Near(out[0], 0) && Near(out[2], 1));
  }

  if (g_failures == 0) {
    std::puts("steering_avoidance: all checks passed");
    return 0;
  }
  std::printf("steering_avoidance: %d checks FAILED\n", g_failures);
  return 1;
}
