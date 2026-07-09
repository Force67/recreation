#ifndef RECREATION_WORLD_PATHFIND_H_
#define RECREATION_WORLD_PATHFIND_H_

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <vector>

namespace rx::world {

struct PathNode {
  int x = 0;
  int y = 0;
};

// A* over a `width` x `height` grid of cells, 8-connected. A cell is impassable
// when `blocked(x, y)` returns true. Diagonal moves are allowed only when both
// shared orthogonal neighbors are open (no cutting through wall corners).
// Finds the cheapest path from `start` to `goal` and writes the cell sequence
// from start to goal inclusive into `out_path`, returning true. Returns false
// (and leaves out_path empty) when: an endpoint is out of [0,width) x [0,height)
// or blocked, or no path exists. `max_visited` caps the number of expanded
// nodes (0 = unbounded) so a caller can bound the cost on a large grid.
inline bool FindPath(int width, int height, const std::function<bool(int, int)>& blocked,
                     PathNode start, PathNode goal, std::vector<PathNode>* out_path,
                     int max_visited = 0) {
  out_path->clear();
  auto in_range = [&](int x, int y) { return x >= 0 && x < width && y >= 0 && y < height; };
  if (!in_range(start.x, start.y) || !in_range(goal.x, goal.y)) return false;
  if (blocked(start.x, start.y) || blocked(goal.x, goal.y)) return false;
  if (start.x == goal.x && start.y == goal.y) {
    out_path->push_back(start);
    return true;
  }

  const float kDiag = std::sqrt(2.0f);
  auto octile = [&](int x, int y) {
    const float dx = std::fabs(static_cast<float>(x - goal.x));
    const float dy = std::fabs(static_cast<float>(y - goal.y));
    return (dx + dy) + (kDiag - 2.0f) * std::fmin(dx, dy);
  };
  auto idx = [&](int x, int y) { return y * width + x; };

  const float kInf = std::numeric_limits<float>::infinity();
  std::vector<float> g(static_cast<size_t>(width) * height, kInf);
  std::vector<int> parent(static_cast<size_t>(width) * height, -1);
  std::vector<bool> closed(static_cast<size_t>(width) * height, false);

  struct Open {
    float f;
    int x, y;
    bool operator>(const Open& o) const { return f > o.f; }
  };
  std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
  g[idx(start.x, start.y)] = 0.0f;
  open.push({octile(start.x, start.y), start.x, start.y});

  int visited = 0;
  while (!open.empty()) {
    const Open cur = open.top();
    open.pop();
    const int ci = idx(cur.x, cur.y);
    if (closed[ci]) continue;
    closed[ci] = true;
    if (max_visited > 0 && ++visited > max_visited) return false;

    if (cur.x == goal.x && cur.y == goal.y) {
      for (int i = ci; i != -1; i = parent[i]) out_path->push_back({i % width, i / width});
      std::reverse(out_path->begin(), out_path->end());
      return true;
    }

    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const int nx = cur.x + dx;
        const int ny = cur.y + dy;
        if (!in_range(nx, ny) || blocked(nx, ny)) continue;
        if (dx != 0 && dy != 0 &&
            (blocked(cur.x + dx, cur.y) || blocked(cur.x, cur.y + dy)))
          continue;
        const int ni = idx(nx, ny);
        if (closed[ni]) continue;
        const float step = (dx != 0 && dy != 0) ? kDiag : 1.0f;
        const float ng = g[ci] + step;
        if (ng < g[ni]) {
          g[ni] = ng;
          parent[ni] = ci;
          open.push({ng + octile(nx, ny), nx, ny});
        }
      }
    }
  }
  return false;
}

}  // namespace rx::world

#endif  // RECREATION_WORLD_PATHFIND_H_
