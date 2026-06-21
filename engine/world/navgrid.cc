#include "world/navgrid.h"

#include <algorithm>
#include <cmath>

#include "world/pathfind.h"

namespace rec::world {

void NavGrid::Build(const Vec3& center, f32 half_extent_m, f32 cell_m, f32 walk_height,
                    f32 step_tolerance, const FloorProbe& probe) {
  walkable_.clear();
  floor_y_.clear();
  width_ = 0;

  if (cell_m <= 0.0f || half_extent_m <= 0.0f || !probe) return;

  cell_m_ = cell_m;
  walk_height_ = walk_height;
  // Cells span the whole region; round up so the full half-extent is covered.
  width_ = static_cast<int>(std::ceil((2.0f * half_extent_m) / cell_m));
  if (width_ < 1) width_ = 1;
  // Center the cell band on `center` so cell (cx,cz) center sits at
  // origin + (cx + 0.5) * cell along each axis.
  const f32 span = width_ * cell_m;
  origin_x_ = center.x - span * 0.5f;
  origin_z_ = center.z - span * 0.5f;

  const size_t count = static_cast<size_t>(width_) * static_cast<size_t>(width_);
  walkable_.assign(count, false);
  floor_y_.assign(count, walk_height);

  for (int cz = 0; cz < width_; ++cz) {
    for (int cx = 0; cx < width_; ++cx) {
      const Vec3 p = CellToWorld(cx, cz);
      f32 floor = 0;
      if (probe(p.x, p.z, &floor) && std::fabs(floor - walk_height) <= step_tolerance) {
        const int i = Index(cx, cz);
        walkable_[i] = true;
        floor_y_[i] = floor;
      }
    }
  }
}

bool NavGrid::Covers(const Vec3& p) const {
  if (Empty()) return false;
  // Shrink the region by one cell so a query near the edge rebuilds before the
  // route would step off the grid.
  const f32 margin = cell_m_;
  const f32 max_x = origin_x_ + width_ * cell_m_;
  const f32 max_z = origin_z_ + width_ * cell_m_;
  return p.x >= origin_x_ + margin && p.x <= max_x - margin && p.z >= origin_z_ + margin &&
         p.z <= max_z - margin;
}

bool NavGrid::Walkable(const Vec3& p) const {
  int cx = 0, cz = 0;
  if (!WorldToCell(p, &cx, &cz)) return false;
  return walkable_[Index(cx, cz)];
}

Vec3 NavGrid::Next(const Vec3& from, const Vec3& goal, int lookahead_cells) const {
  if (Empty()) return goal;

  int sx = 0, sz = 0, gx = 0, gz = 0;
  if (!WorldToCell(from, &sx, &sz) || !WorldToCell(goal, &gx, &gz)) return goal;

  const auto blocked = [this](int x, int z) { return !walkable_[Index(x, z)]; };
  std::vector<PathNode> path;
  if (!FindPath(width_, width_, blocked, {sx, sz}, {gx, gz}, &path) || path.empty()) {
    return goal;
  }

  // Step a few cells along the route; clamp to the last node. The first node is
  // the start cell, so a single-cell path (start == goal) returns the goal.
  const int step = std::min(std::max(lookahead_cells, 1), static_cast<int>(path.size()) - 1);
  if (step <= 0) return goal;
  const PathNode& wp = path[static_cast<size_t>(step)];
  return CellToWorld(wp.x, wp.y);
}

bool NavGrid::WorldToCell(const Vec3& p, int* cx, int* cz) const {
  if (Empty()) return false;
  const int x = static_cast<int>(std::floor((p.x - origin_x_) / cell_m_));
  const int z = static_cast<int>(std::floor((p.z - origin_z_) / cell_m_));
  if (x < 0 || x >= width_ || z < 0 || z >= width_) return false;
  *cx = x;
  *cz = z;
  return true;
}

Vec3 NavGrid::CellToWorld(int cx, int cz) const {
  const f32 x = origin_x_ + (static_cast<f32>(cx) + 0.5f) * cell_m_;
  const f32 z = origin_z_ + (static_cast<f32>(cz) + 0.5f) * cell_m_;
  f32 y = walk_height_;
  if (!Empty() && cx >= 0 && cx < width_ && cz >= 0 && cz < width_) y = floor_y_[Index(cx, cz)];
  return {x, y, z};
}

}  // namespace rec::world
