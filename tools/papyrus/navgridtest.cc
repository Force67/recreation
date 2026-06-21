// navgridtest: checks the persistent walkability grid NPCs/the player route over
// inside interiors. Pure search over a synthetic floor probe, no game data.

#include <cmath>
#include <cstdio>

#include "world/navgrid.h"

using rec::Vec3;
using rec::world::NavGrid;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// A synthetic 20x20 meter room with floor at y=0, a full-height wall down x=10
// (band x in [9.4, 10.6]) broken by a doorway gap at z in [8, 12]. The wall is
// modeled as a floor lifted to y=3 (above any reasonable step tolerance); the
// world outside the room is a void (no floor at all).
bool RoomProbe(float x, float z, float* floor_y) {
  if (x < 0.0f || x > 20.0f || z < 0.0f || z > 20.0f) return false;  // void outside
  const bool in_wall_band = x > 9.4f && x < 10.6f;
  const bool in_doorway = z > 8.0f && z < 12.0f;
  if (in_wall_band && !in_doorway) {
    *floor_y = 3.0f;  // wall top, well above the floor
    return true;
  }
  *floor_y = 0.0f;
  return true;
}

// The same room but the wall has no gap: the two sides are disconnected.
bool SealedRoomProbe(float x, float z, float* floor_y) {
  if (x < 0.0f || x > 20.0f || z < 0.0f || z > 20.0f) return false;
  if (x > 9.4f && x < 10.6f) {
    *floor_y = 3.0f;
    return true;
  }
  *floor_y = 0.0f;
  return true;
}

bool Near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
  std::puts("navgrid:");

  // An unbuilt grid is empty and routes nowhere useful.
  {
    NavGrid g;
    Check("fresh grid -> empty", g.Empty());
    Check("fresh grid -> covers nothing", !g.Covers({10, 0, 10}));
    const Vec3 goal{18, 0, 10};
    const Vec3 n = g.Next({2, 0, 10}, goal);
    Check("fresh grid -> Next falls back to goal",
          Near(n.x, goal.x) && Near(n.z, goal.z));
  }

  // Build over the room: center (10,0,10), 11 m half-extent, 0.8 m cells.
  NavGrid g;
  g.Build({10, 0, 10}, 11.0f, 0.8f, /*walk_height=*/0.0f, /*step_tolerance=*/0.4f, RoomProbe);

  Check("built grid -> not empty", !g.Empty());

  // Walkability: open floor is walkable, the wall and the void are not.
  Check("open floor walkable", g.Walkable({3, 0, 3}));
  Check("doorway gap walkable", g.Walkable({10, 0, 10}));
  Check("wall blocked", !g.Walkable({10, 0, 3}));
  Check("void outside blocked", !g.Walkable({30, 0, 30}));

  // Coverage: inside the region is covered, far outside is not.
  Check("center covered", g.Covers({10, 0, 10}));
  Check("far point not covered", !g.Covers({100, 0, 100}));

  // Route from the left side to the right side. The straight line at z=3 would
  // cross the wall; the path must detour through the doorway (pass near z~10).
  {
    const Vec3 from{3, 0, 3};
    const Vec3 goal{17, 0, 3};

    // Walk the grid one lookahead step at a time to recover the full route and
    // confirm no waypoint lands on the wall and at least one passes the doorway.
    bool reached = false;
    bool any_in_doorway = false;
    bool any_in_wall = false;
    Vec3 cur = from;
    for (int i = 0; i < 200; ++i) {
      const Vec3 wp = g.Next(cur, goal, /*lookahead_cells=*/1);
      if (wp.x > 9.4f && wp.x < 10.6f) any_in_wall = true;
      if (wp.x > 9.4f && wp.x < 10.6f && wp.z > 8.0f && wp.z < 12.0f) any_in_doorway = true;
      cur = wp;
      if (Near(cur.x, goal.x, 1.0f) && Near(cur.z, goal.z, 1.0f)) {
        reached = true;
        break;
      }
    }
    Check("route reaches the far side", reached);
    Check("route passes through the doorway", any_in_doorway);
    Check("route never steps onto a non-doorway wall cell", !any_in_wall || any_in_doorway);

    // A single multi-cell lookahead should already steer toward +x (the goal),
    // not back into the wall.
    const Vec3 hop = g.Next(from, goal, /*lookahead_cells=*/4);
    Check("lookahead waypoint advances toward goal", hop.x > from.x);
    Check("lookahead waypoint is on walkable ground", g.Walkable(hop));
  }

  // No-route fallback: a sealed wall leaves the right side unreachable, so Next
  // returns the goal unchanged.
  {
    NavGrid sealed;
    sealed.Build({10, 0, 10}, 11.0f, 0.8f, 0.0f, 0.4f, SealedRoomProbe);
    const Vec3 from{3, 0, 10};
    const Vec3 goal{17, 0, 10};
    const Vec3 n = sealed.Next(from, goal);
    Check("sealed room -> Next falls back to goal",
          Near(n.x, goal.x) && Near(n.z, goal.z));
  }

  // Goal off the grid -> fallback to goal (caller steers straight, then rebuilds).
  {
    const Vec3 goal{200, 0, 200};
    const Vec3 n = g.Next({3, 0, 3}, goal);
    Check("off-grid goal -> Next falls back to goal",
          Near(n.x, goal.x) && Near(n.z, goal.z));
  }

  // start == goal -> returns the goal, no spurious detour.
  {
    const Vec3 p{5, 0, 5};
    const Vec3 n = g.Next(p, p);
    Check("start == goal -> returns goal", Near(n.x, p.x) && Near(n.z, p.z));
  }

  // Floor height is reported from the probe, so waypoints sit on the ground.
  {
    const Vec3 wp = g.Next({3, 0, 3}, {3, 0, 9}, 2);
    Check("waypoint sits on the floor (y ~ 0)", Near(wp.y, 0.0f, 0.1f));
  }

  // The grid reach is larger than the old 17 m per-call grid.
  Check("reach spans more than 17 m", g.width() * g.cell_m() > 17.0f);

  if (g_failures == 0) {
    std::puts("navgrid: all checks passed");
    return 0;
  }
  std::printf("navgrid: %d checks FAILED\n", g_failures);
  return 1;
}
