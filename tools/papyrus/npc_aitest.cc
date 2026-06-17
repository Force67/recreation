// npc_aitest: checks the planar NPC steering helpers (seek-with-arrival,
// follow-slot formation, separation). Pure geometry, no game data.

#include <cmath>
#include <cstdio>

#include "world/npc_ai.h"

using rec::world::FollowSlot;
using rec::world::SeparationOffset;
using rec::world::SteerOutput;
using rec::world::SteerParams;
using rec::world::SteerToward;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
  const SteerParams params;  // speed 3, arrive 2.5, stop 1.4
  const float origin[3] = {0, 0, 0};

  std::puts("npc steering:");

  // Inside stop_radius -> arrived and standing still.
  {
    const float goal[3] = {1, 0, 0};  // 1m away, under stop_radius 1.4
    SteerOutput o = SteerToward(origin, goal, params);
    Check("inside stop_radius -> arrived", o.arrived);
    Check("arrived -> zero velocity",
          Near(o.velocity[0], 0) && Near(o.velocity[1], 0) && Near(o.velocity[2], 0));
  }
  // Goal due +Z: yaw ~ 0 and velocity points +Z at cruise speed.
  {
    const float goal[3] = {0, 0, 10};
    SteerOutput o = SteerToward(origin, goal, params);
    Check("goal +Z -> not arrived", !o.arrived);
    Check("goal +Z -> yaw ~ 0", Near(o.yaw, 0));
    Check("goal +Z -> +Z velocity at speed",
          Near(o.velocity[0], 0) && Near(o.velocity[1], 0) && Near(o.velocity[2], params.speed));
  }
  // Goal due +X: yaw ~ pi/2 and velocity points +X.
  {
    const float goal[3] = {10, 0, 0};
    SteerOutput o = SteerToward(origin, goal, params);
    Check("goal +X -> yaw ~ pi/2", Near(o.yaw, 3.14159265f / 2));
    Check("goal +X -> +X velocity at speed",
          Near(o.velocity[0], params.speed) && Near(o.velocity[2], 0));
  }
  // Within arrive_radius (but past stop_radius) -> speed scaled down linearly.
  {
    const float goal[3] = {0, 0, 2};  // dist 2, between stop 1.4 and arrive 2.5
    SteerOutput o = SteerToward(origin, goal, params);
    const float want = params.speed * (2.0f / params.arrive_radius);
    Check("inside arrive_radius -> slowed", Near(o.velocity[2], want));
    Check("slowed speed < cruise", o.velocity[2] < params.speed);
  }
  // FollowSlot: leader facing +Z (yaw 0), slot 0 sits directly behind at -Z.
  {
    const float leader[3] = {0, 5, 0};
    float goal[3] = {0, 0, 0};
    FollowSlot(leader, 0.0f, 0, 2.0f, goal);
    Check("slot 0 behind -> -Z", goal[2] < leader[2] && Near(goal[0], 0));
    Check("slot 0 -> spacing behind", Near(goal[2], -2.0f));
    Check("slot keeps leader height", Near(goal[1], leader[1]));
  }
  // Separation: two followers on the same spot push apart (nonzero offset).
  {
    const float self_pos[3] = {0, 0, 0};
    const float others[3] = {0, 0, 0};  // exact overlap
    float off[3] = {0, 0, 0};
    SeparationOffset(self_pos, others, 1, 2.0f, off);
    Check("overlap -> nonzero push", off[0] != 0.0f || off[2] != 0.0f);
    Check("overlap push planar (y=0)", Near(off[1], 0));
  }
  // Separation: a neighbor beyond radius contributes nothing.
  {
    const float self_pos[3] = {0, 0, 0};
    const float others[3] = {10, 0, 0};  // far away
    float off[3] = {1, 1, 1};
    SeparationOffset(self_pos, others, 1, 2.0f, off);
    Check("out of range -> zero offset", Near(off[0], 0) && Near(off[1], 0) && Near(off[2], 0));
  }

  if (g_failures == 0) {
    std::puts("npc_ai: all checks passed");
    return 0;
  }
  std::printf("npc_ai: %d checks FAILED\n", g_failures);
  return 1;
}
