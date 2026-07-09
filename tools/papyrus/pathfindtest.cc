// pathfindtest: checks the A* grid pathfinder NPCs use to route around walls.
// Pure search over small hand-built mazes, no game data.

#include <cstdio>
#include <string>
#include <vector>

#include "world/pathfind.h"

using rx::world::FindPath;
using rx::world::PathNode;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Builds a `blocked` lambda over a maze where '#' is a wall and any other char
// is open. Rows are top (y=0) to bottom; columns are x.
struct Maze {
  std::vector<std::string> rows;
  int width() const { return static_cast<int>(rows[0].size()); }
  int height() const { return static_cast<int>(rows.size()); }
  bool blocked(int x, int y) const { return rows[y][x] == '#'; }
};

// True when no cell on the path sits on a wall.
bool NoneBlocked(const Maze& m, const std::vector<PathNode>& path) {
  for (const PathNode& p : path)
    if (m.blocked(p.x, p.y)) return false;
  return true;
}

// True when the path never steps diagonally between two cells whose two shared
// orthogonal neighbors are both blocked (i.e. never cuts a wall corner).
bool NoCornerCut(const Maze& m, const std::vector<PathNode>& path) {
  for (size_t i = 1; i < path.size(); ++i) {
    const int dx = path[i].x - path[i - 1].x;
    const int dy = path[i].y - path[i - 1].y;
    if (dx != 0 && dy != 0 && m.blocked(path[i - 1].x + dx, path[i - 1].y) &&
        m.blocked(path[i - 1].x, path[i - 1].y + dy))
      return false;
  }
  return true;
}

}  // namespace

int main() {
  std::puts("a* pathfinder:");

  // Straight diagonal across an empty grid: Chebyshev distance + 1 cells.
  {
    Maze m{{".....", ".....", ".....", ".....", "....."}};
    auto blk = [&](int x, int y) { return m.blocked(x, y); };
    std::vector<PathNode> path;
    bool ok = FindPath(m.width(), m.height(), blk, {0, 0}, {4, 4}, &path);
    Check("empty grid -> path found", ok);
    Check("empty grid -> length is chebyshev + 1", path.size() == 5);
    Check("empty grid -> endpoints match",
          ok && path.front().x == 0 && path.front().y == 0 && path.back().x == 4 &&
              path.back().y == 4);
  }

  // A vertical wall between start and goal forces a detour around its end.
  {
    Maze m{{".#...", ".#...", ".#...", ".#...", "....."}};
    auto blk = [&](int x, int y) { return m.blocked(x, y); };
    std::vector<PathNode> path;
    bool ok = FindPath(m.width(), m.height(), blk, {0, 0}, {2, 0}, &path);
    Check("walled line -> path found", ok);
    Check("walled line -> longer than straight line", path.size() > 3);
    Check("walled line -> no path cell is a wall", ok && NoneBlocked(m, path));
    Check("walled line -> no corner cut", ok && NoCornerCut(m, path));
  }

  // Goal sealed off by a ring of walls: unreachable.
  {
    Maze m{{".....", ".###.", ".#G#.", ".###.", "....."}};
    auto blk = [&](int x, int y) { return m.blocked(x, y); };
    std::vector<PathNode> path;
    bool ok = FindPath(m.width(), m.height(), blk, {0, 0}, {2, 2}, &path);
    Check("walled-off goal -> no path", !ok);
    Check("walled-off goal -> out_path empty", path.empty());
  }

  // Blocked endpoints and out-of-range endpoints are rejected.
  {
    Maze m{{".#...", "....."}};
    auto blk = [&](int x, int y) { return m.blocked(x, y); };
    std::vector<PathNode> path;
    Check("blocked goal -> false", !FindPath(m.width(), m.height(), blk, {0, 0}, {1, 0}, &path));
    Check("blocked goal -> out_path empty", path.empty());
    Check("blocked start -> false", !FindPath(m.width(), m.height(), blk, {1, 0}, {4, 1}, &path));
    Check("out-of-range goal -> false",
          !FindPath(m.width(), m.height(), blk, {0, 0}, {9, 9}, &path));
    Check("out-of-range start -> false",
          !FindPath(m.width(), m.height(), blk, {-1, 0}, {4, 1}, &path));
  }

  // start == goal on an open cell yields a single-element path.
  {
    Maze m{{"...", "...", "..."}};
    auto blk = [&](int x, int y) { return m.blocked(x, y); };
    std::vector<PathNode> path;
    bool ok = FindPath(m.width(), m.height(), blk, {1, 1}, {1, 1}, &path);
    Check("start == goal -> single cell", ok && path.size() == 1 && path[0].x == 1 &&
                                              path[0].y == 1);
  }

  // Corner cutting prevented: walls at (2,1) and (1,2) wall off the direct
  // diagonal from (1,1) to (2,2); the only legal route is around, never through.
  {
    Maze m{{"....", ".#.#", "..#.", "...."}};  // walls at (1,1),(3,1),(2,2)
    auto blk = [&](int x, int y) { return m.blocked(x, y); };
    std::vector<PathNode> path;
    bool ok = FindPath(m.width(), m.height(), blk, {1, 2}, {2, 1}, &path);
    Check("corner -> path found around", ok);
    Check("corner -> no diagonal through the wall corner", ok && NoCornerCut(m, path));
    Check("corner -> longer than a single diagonal", path.size() > 2);
  }

  // On a 2x2 grid, blocking the two orthogonal neighbors of (1,1) makes the
  // only approach an illegal corner-cutting diagonal, so the goal is unreachable.
  {
    auto blk = [](int x, int y) { return (x == 1 && y == 0) || (x == 0 && y == 1); };
    std::vector<PathNode> path;
    bool ok = FindPath(2, 2, blk, {0, 0}, {1, 1}, &path);
    Check("enclosed corner goal -> no path", !ok);
  }

  // max_visited forces a bounded give-up on a large open grid.
  {
    auto blk = [](int, int) { return false; };
    std::vector<PathNode> path;
    bool ok = FindPath(100, 100, blk, {0, 0}, {99, 99}, &path, /*max_visited=*/8);
    Check("max_visited small -> bounded give-up", !ok);
    Check("max_visited give-up -> out_path empty", path.empty());
  }

  // Same large grid unbounded still routes corner to corner.
  {
    auto blk = [](int, int) { return false; };
    std::vector<PathNode> path;
    bool ok = FindPath(100, 100, blk, {0, 0}, {99, 99}, &path);
    Check("large grid unbounded -> path found", ok && path.size() == 100);
  }

  if (g_failures == 0) {
    std::puts("pathfind: all checks passed");
    return 0;
  }
  std::printf("pathfind: %d checks FAILED\n", g_failures);
  return 1;
}
