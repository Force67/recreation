#ifndef RECREATION_WORLD_NAVGRID_H_
#define RECREATION_WORLD_NAVGRID_H_

#include <functional>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace rec::world {

// A persistent walkability grid over a square region of the world, sampled once
// from a caller-supplied floor probe and reused across many path queries. It
// replaces the per-call O(n^2) raycast grid NPC steering used to rebuild every
// frame: build once over a room (e.g. the Helgen keep), then call Next() until
// the actor walks out of the covered area and a rebuild is needed.
//
// The grid is on the XZ plane (y is up, matching the engine). A cell is walkable
// when the probe reports a floor within `step_tolerance` of the build height;
// cells over a void, ledge, or wall top are blocked. Routing is 8-connected A*
// reusing world::FindPath, so it will not cut wall corners.
class NavGrid {
 public:
  // probe(x, z, &floor_y): returns true and writes the floor height at world
  // (x, z) when solid ground is found there, false over a gap. Engine-agnostic
  // so the runtime can back it with a downward physics raycast and tests can
  // back it with a synthetic room layout.
  using FloorProbe = std::function<bool(f32 x, f32 z, f32* floor_y)>;

  // (Re)builds the grid covering [center.xz - half_extent, center.xz +
  // half_extent] at `cell_m` resolution. A cell is walkable when the probe finds
  // a floor within `step_tolerance` meters of `walk_height` (the actor's foot
  // height, typically center.y). Sampling is one probe per cell, paid once.
  void Build(const Vec3& center, f32 half_extent_m, f32 cell_m, f32 walk_height,
             f32 step_tolerance, const FloorProbe& probe);

  // True when `p` lies inside the built region shrunk by one cell, so a query
  // near the edge triggers a rebuild before the route runs off the grid.
  bool Covers(const Vec3& p) const;

  // True before Build() or after a Build() that produced no cells.
  bool Empty() const { return walkable_.empty(); }

  // A waypoint to steer toward to reach `goal`: the cell `lookahead_cells` along
  // the A* route from `from`, returned at its sampled floor height. Returns
  // `goal` unchanged when there is no usable route (grid empty, endpoint off the
  // grid or blocked, or no path), so the caller can fall back to steering
  // straight at the goal.
  Vec3 Next(const Vec3& from, const Vec3& goal, int lookahead_cells = 4) const;

  // Whether the cell containing world point `p` is walkable. False off-grid.
  bool Walkable(const Vec3& p) const;

  int width() const { return width_; }
  f32 cell_m() const { return cell_m_; }

 private:
  // Writes the cell coordinate containing world point `p`. Returns false when
  // `p` is outside the built region.
  bool WorldToCell(const Vec3& p, int* cx, int* cz) const;
  Vec3 CellToWorld(int cx, int cz) const;
  int Index(int cx, int cz) const { return cz * width_ + cx; }

  // Min corner (cell 0,0 center is origin_ + half a cell) and dimensions.
  f32 origin_x_ = 0;  // world x of the region min corner
  f32 origin_z_ = 0;  // world z of the region min corner
  f32 cell_m_ = 1;
  int width_ = 0;   // cells per side (square grid)
  f32 walk_height_ = 0;

  std::vector<bool> walkable_;
  std::vector<f32> floor_y_;
};

}  // namespace rec::world

#endif  // RECREATION_WORLD_NAVGRID_H_
