// objective_markertest: the pure geometry behind objective waypoints, the reach
// test and the compass bearing the HUD pip uses. No game data.

#include <cmath>
#include <cstdio>

#include "world/objective_marker.h"

using rx::world::MarkerCompassBearingDeg;
using rx::world::MarkerReached;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) < eps; }

}  // namespace

int main() {
  std::puts("marker reach:");
  {
    const float p[3] = {0, 0, 0};
    const float inside[3] = {1.5f, 0, 0};
    const float outside[3] = {5, 0, 0};
    Check("inside radius -> reached", MarkerReached(p, inside, 2.5f));
    Check("outside radius -> not reached", !MarkerReached(p, outside, 2.5f));
    // Height is ignored: a marker far overhead still triggers if XZ is close.
    const float overhead[3] = {1.0f, 50.0f, 0};
    Check("height ignored", MarkerReached(p, overhead, 2.5f));
    // ...but XZ distance is respected at the same height.
    const float to_side[3] = {3.0f, 0, 0};
    Check("xz distance respected", !MarkerReached(p, to_side, 2.5f));
    // Exactly on the edge counts as reached (<=).
    const float on_edge[3] = {2.5f, 0, 0};
    Check("on the radius edge -> reached", MarkerReached(p, on_edge, 2.5f));
  }

  std::puts("compass bearing (looking down +Z):");
  {
    const float fwd[3] = {0, 0, 1};
    float to[3];
    auto bearing = [&](float x, float z) {
      to[0] = x;
      to[1] = 0;
      to[2] = z;
      return MarkerCompassBearingDeg(fwd, to);
    };
    Check("dead ahead -> 0", Near(bearing(0, 5), 0.0f));
    Check("to the right -> +90", Near(bearing(5, 0), 90.0f));
    Check("to the left -> -90", Near(bearing(-5, 0), -90.0f));
    Check("behind -> 180", Near(std::fabs(bearing(0, -5)), 180.0f));
    Check("ahead and right -> +45", Near(bearing(5, 5), 45.0f));
    // Magnitude does not matter, only direction.
    Check("unnormalized input ok", Near(bearing(100, 0), 90.0f));
  }

  std::puts("compass bearing (arbitrary facing):");
  {
    const float fwd_x[3] = {1, 0, 0};  // looking down +X
    float ahead[3] = {5, 0, 0};
    Check("ahead -> 0", Near(MarkerCompassBearingDeg(fwd_x, ahead), 0.0f));
    float right[3] = {0, 0, -5};  // +X facing, right hand turns toward -Z
    Check("right -> +90", Near(MarkerCompassBearingDeg(fwd_x, right), 90.0f));
    const float zero[3] = {0, 0, 0};
    Check("degenerate marker -> 0", Near(MarkerCompassBearingDeg(fwd_x, zero), 0.0f));
    Check("degenerate facing -> 0", Near(MarkerCompassBearingDeg(zero, ahead), 0.0f));
  }

  if (g_failures == 0) {
    std::puts("objective_marker: all checks passed");
    return 0;
  }
  std::printf("objective_marker: %d checks FAILED\n", g_failures);
  return 1;
}
