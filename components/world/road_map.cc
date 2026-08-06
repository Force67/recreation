#include "components/world/road_map.h"

#include <base/algorithm.h>
#include <base/strings/xstring.h>

#include <cctype>
#include <cmath>
#include <cstring>

#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"

namespace rx::world {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kBtxt = FourCc('B', 'T', 'X', 'T');
constexpr u32 kAtxt = FourCc('A', 'T', 'X', 'T');
constexpr u32 kVtxt = FourCc('V', 'T', 'X', 'T');

constexpr u32 kQuadGrid = 17;    // VTXT opacity grid per quadrant
constexpr f32 kCellSize = 4096;  // game units across an exterior cell
// What a step across open country costs against the same step on a road. High
// enough that the search will go a long way round to stay surfaced, low enough
// that it still crosses country where a road does not go.
constexpr f32 kOffRoadCost = 16.0f;
// The search is after a good road, not the provably shortest one, so the
// estimate of what is left is inflated. Without this an off-road step costing
// several times an on-road one leaves the plain distance estimate so loose that
// the search fans out over the whole county before it commits to a direction.
constexpr f32 kHeuristicWeight = 4.0f;

// One texture layer over a quadrant, with its per-vertex opacity.
struct QuadLayer {
  bool road = false;
  u32 quadrant = 0;
  f32 opacity[kQuadGrid * kQuadGrid] = {};
};

base::String Lowered(const base::String& s) {
  base::String out = s;
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

bool RoadMap::IsRoadTexture(const bethesda::RecordStore& records, bethesda::GlobalFormId ltex) {
  bethesda::Record record;
  if (!records.Parse(ltex, &record))
    return false;
  const base::String name = Lowered(record.GetString(kEdid));
  // Named for what they are, and what the games surface a road with.
  for (const char* mark : {"dirt", "path", "road", "cobble"})
    if (name.find(mark) != base::String::npos)
      return true;
  return false;
}

u32 RoadMap::Key(i16 grid_x, i16 grid_y) {
  return static_cast<u32>(static_cast<u16>(grid_x)) << 16 | static_cast<u16>(grid_y);
}

void RoadMap::AddCell(const bethesda::RecordStore& records, u64 land_ref, i16 grid_x, i16 grid_y) {
  const bethesda::GlobalFormId land_id{static_cast<u16>(land_ref >> 32),
                                       static_cast<u32>(land_ref)};
  bethesda::Record land;
  const bethesda::RecordStore::StoredRecord* stored = records.Find(land_id);
  if (!stored || !records.Parse(land_id, &land))
    return;
  const u16 plugin = stored->winning_plugin;

  auto is_road = [&](u32 raw) {
    if (raw == 0)
      return false;  // the worldspace default ground, never a road
    const bethesda::GlobalFormId ltex = records.ResolveFrom(bethesda::RawFormId{raw}, plugin);
    if (const bool* known = road_textures_.find(ltex.packed()))
      return *known;
    const bool road = IsRoadTexture(records, ltex);
    road_textures_.insert(ltex.packed(), road);
    return road;
  };

  // Walk the subrecord stream: BTXT sets a quadrant's base, ATXT opens a layer
  // whose VTXT follows.
  bool base_road[4] = {};
  base::Vector<QuadLayer> layers;
  QuadLayer* open = nullptr;
  for (const bethesda::Subrecord& sub : land.subrecords) {
    if ((sub.type == kBtxt || sub.type == kAtxt) && sub.data.size() >= 8) {
      u32 raw = 0;
      std::memcpy(&raw, sub.data.data(), 4);
      const u8 quadrant = sub.data[4];
      if (quadrant > 3) {
        open = nullptr;
        continue;
      }
      if (sub.type == kBtxt) {
        base_road[quadrant] = is_road(raw);
        open = nullptr;
      } else {
        QuadLayer layer;
        layer.road = is_road(raw);
        layer.quadrant = quadrant;
        layers.push_back(layer);
        open = &layers.back();
      }
    } else if (sub.type == kVtxt && open) {
      for (size_t i = 0; i + 8 <= sub.data.size(); i += 8) {
        u16 point = 0;
        f32 opacity = 0;
        std::memcpy(&point, sub.data.data() + i, 2);
        std::memcpy(&opacity, sub.data.data() + i + 4, 4);
        if (point < kQuadGrid * kQuadGrid)
          open->opacity[point] = base::Clamp(opacity, 0.0f, 1.0f);
      }
      open = nullptr;
    }
  }

  // Resolve to a weight grid the same way the splat bake does, so what steers
  // over a road is the same surface that is drawn as one.
  Cell cell;
  constexpr u32 kHalf = kResolution / 2;
  for (u32 ty = 0; ty < kResolution; ++ty) {
    const u32 qy = ty >= kHalf ? ty - kHalf : ty;
    const f32 gy = (static_cast<f32>(qy) + 0.5f) / kHalf * (kQuadGrid - 1);
    const u32 cy = base::Min(static_cast<u32>(gy), kQuadGrid - 2);
    const f32 fy = gy - static_cast<f32>(cy);
    for (u32 tx = 0; tx < kResolution; ++tx) {
      const u32 quadrant = (tx >= kHalf ? 1u : 0u) | (ty >= kHalf ? 2u : 0u);
      const u32 qx = tx >= kHalf ? tx - kHalf : tx;
      const f32 gx = (static_cast<f32>(qx) + 0.5f) / kHalf * (kQuadGrid - 1);
      const u32 cx = base::Min(static_cast<u32>(gx), kQuadGrid - 2);
      const f32 fx = gx - static_cast<f32>(cx);

      f32 road = base_road[quadrant] ? 1.0f : 0.0f;
      for (const QuadLayer& layer : layers) {
        if (layer.quadrant != quadrant)
          continue;
        const f32* o = layer.opacity;
        const f32 o00 = o[cy * kQuadGrid + cx], o10 = o[cy * kQuadGrid + cx + 1];
        const f32 o01 = o[(cy + 1) * kQuadGrid + cx], o11 = o[(cy + 1) * kQuadGrid + cx + 1];
        const f32 op = (o00 * (1 - fx) + o10 * fx) * (1 - fy) + (o01 * (1 - fx) + o11 * fx) * fy;
        if (op <= 0.001f)
          continue;
        road = road * (1 - op) + (layer.road ? op : 0.0f);
      }
      cell.weight[ty * kResolution + tx] = base::Clamp(road, 0.0f, 1.0f);
    }
  }
  cells_.insert(Key(grid_x, grid_y), cell);
}

void RoadMap::RemoveCell(i16 grid_x, i16 grid_y) {
  cells_.erase(Key(grid_x, grid_y));
}

bool RoadMap::Has(i16 grid_x, i16 grid_y) const {
  return cells_.find(Key(grid_x, grid_y)) != nullptr;
}

void RoadMap::SetSource(const bethesda::RecordStore* records, bethesda::GlobalFormId worldspace) {
  source_ = records;
  worldspace_ = worldspace;
}

const RoadMap::Cell* RoadMap::Ensure(i16 grid_x, i16 grid_y) {
  const u32 key = Key(grid_x, grid_y);
  if (const Cell* known = cells_.find(key))
    return known;
  if (!source_ || looked_at_.find(key))
    return nullptr;
  looked_at_.insert(key, true);
  const bethesda::RecordStore::ExteriorGrid* grid = source_->ExteriorCells(worldspace_);
  const bethesda::RecordStore::ExteriorCell* cell =
      grid ? grid->find(bethesda::RecordStore::GridKey(grid_x, grid_y)) : nullptr;
  if (!cell || !cell->land)
    return nullptr;
  AddCell(*source_, cell->land, grid_x, grid_y);
  return cells_.find(key);
}

f32 RoadMap::At(f32 game_x, f32 game_y) {
  const f32 cell_x = std::floor(game_x / kCellSize);
  const f32 cell_y = std::floor(game_y / kCellSize);
  const Cell* cell = Ensure(static_cast<i16>(cell_x), static_cast<i16>(cell_y));
  if (!cell)
    return 0;
  const f32 local_x = (game_x - cell_x * kCellSize) / kCellSize * kResolution;
  const f32 local_y = (game_y - cell_y * kCellSize) / kCellSize * kResolution;
  const u32 ix = base::Min(static_cast<u32>(base::Max(local_x, 0.0f)), kResolution - 1);
  const u32 iy = base::Min(static_cast<u32>(base::Max(local_y, 0.0f)), kResolution - 1);
  return cell->weight[iy * kResolution + ix];
}

f32 RoadMap::Near(f32 game_x, f32 game_y, f32 radius) {
  constexpr f32 kSample = kCellSize / kResolution;
  const int steps = base::Max(static_cast<int>(radius / kSample), 0);
  f32 most = 0;
  for (int dy = -steps; dy <= steps; ++dy)
    for (int dx = -steps; dx <= steps; ++dx)
      most = base::Max(most, At(game_x + static_cast<f32>(dx) * kSample,
                                game_y + static_cast<f32>(dy) * kSample));
  return most;
}

Vec3 RoadMap::NearestRoad(const Vec3& near, f32 radius) {
  constexpr f32 kSample = kCellSize / kResolution;
  const int steps = base::Max(static_cast<int>(radius / kSample), 1);
  Vec3 best = near;
  f32 best_score = At(near.x, near.y);
  for (int dy = -steps; dy <= steps; ++dy) {
    for (int dx = -steps; dx <= steps; ++dx) {
      const f32 x = near.x + static_cast<f32>(dx) * kSample;
      const f32 y = near.y + static_cast<f32>(dy) * kSample;
      const f32 distance =
          std::sqrt(static_cast<f32>(dx * dx + dy * dy)) * kSample / base::Max(radius, 1.0f);
      // Road first, then whatever is nearest: a strong bit of road a little
      // further off beats a faint one underfoot.
      const f32 score = At(x, y) - distance * 0.35f;
      if (score > best_score) {
        best_score = score;
        best = {x, y, 0};
      }
    }
  }
  return best;
}

base::Vector<Vec3> RoadMap::FindRoute(const Vec3& from, const Vec3& to, f32 stride, u32 budget) {
  base::Vector<Vec3> route;
  const f32 kStep = base::Max(stride, kCellSize / kResolution);
  const i32 start_x = static_cast<i32>(std::floor(from.x / kStep));
  const i32 start_y = static_cast<i32>(std::floor(from.y / kStep));
  const i32 goal_x = static_cast<i32>(std::floor(to.x / kStep));
  const i32 goal_y = static_cast<i32>(std::floor(to.y / kStep));
  if (start_x == goal_x && start_y == goal_y)
    return route;

  auto pack = [](i32 x, i32 y) {
    return static_cast<u64>(static_cast<u32>(x)) << 32 | static_cast<u32>(y);
  };
  // A stride wider than a road would step straight over it, so a sample is the
  // most road anywhere in the square it stands for.
  auto road_at = [&](i32 x, i32 y) {
    return Near((static_cast<f32>(x) + 0.5f) * kStep, (static_cast<f32>(y) + 0.5f) * kStep,
                kStep * 0.5f);
  };
  auto heuristic = [&](i32 x, i32 y) {
    const f32 dx = static_cast<f32>(goal_x - x), dy = static_cast<f32>(goal_y - y);
    return std::sqrt(dx * dx + dy * dy) * kStep * kHeuristicWeight;
  };

  struct Open {
    f32 estimate = 0;
    i32 x = 0, y = 0;
  };
  struct Visited {
    f32 cost = 0;
    i32 from_x = 0, from_y = 0;
  };
  base::Vector<Open> open;
  base::UnorderedMap<u64, Visited> seen;
  open.push_back({heuristic(start_x, start_y), start_x, start_y});
  seen.insert(pack(start_x, start_y), {0, start_x, start_y});

  bool arrived = false;
  u32 expanded = 0;
  while (!open.empty() && expanded < budget) {
    // The frontier stays small enough that a linear scan beats the bookkeeping
    // of a heap here, and it keeps this readable.
    size_t best = 0;
    for (size_t i = 1; i < open.size(); ++i)
      if (open[i].estimate < open[best].estimate)
        best = i;
    const Open node = open[best];
    open[best] = open.back();
    open.pop_back();
    ++expanded;
    if (node.x == goal_x && node.y == goal_y) {
      arrived = true;
      break;
    }
    const Visited* here = seen.find(pack(node.x, node.y));
    if (!here)
      continue;
    const f32 cost_here = here->cost;
    for (i32 dy = -1; dy <= 1; ++dy) {
      for (i32 dx = -1; dx <= 1; ++dx) {
        if (!dx && !dy)
          continue;
        const i32 nx = node.x + dx, ny = node.y + dy;
        const f32 span = (dx && dy) ? kStep * 1.41421356f : kStep;
        // Open country is passable, just dear: the search takes the long way
        // round when the long way is surfaced, and cuts across when it must.
        const f32 surface = road_at(nx, ny);
        const f32 cost = cost_here + span * (1.0f + kOffRoadCost * (1.0f - surface));
        const u64 key = pack(nx, ny);
        if (const Visited* known = seen.find(key); known && known->cost <= cost)
          continue;
        seen.insert(key, {cost, node.x, node.y});
        open.push_back({cost + heuristic(nx, ny), nx, ny});
      }
    }
  }
  if (!arrived)
    return route;

  // Walk the parents back, keeping a corner only where the route turns, so what
  // comes out is a handful of waypoints rather than a sample per 128 units.
  base::Vector<Vec3> reversed;
  i32 x = goal_x, y = goal_y;
  i32 last_dx = 0, last_dy = 0;
  while (true) {
    const Visited* step = seen.find(pack(x, y));
    if (!step)
      break;
    const i32 dx = x - step->from_x, dy = y - step->from_y;
    const bool corner = dx != last_dx || dy != last_dy;
    if (corner || reversed.empty())
      reversed.push_back({(static_cast<f32>(x) + 0.5f) * kStep, (static_cast<f32>(y) + 0.5f) * kStep,
                          0});
    last_dx = dx;
    last_dy = dy;
    if (step->from_x == x && step->from_y == y)
      break;
    x = step->from_x;
    y = step->from_y;
  }
  // A coarse stride finds the road but lands its corners anywhere in the square
  // that held it, so each one is pulled onto the most road within a stride.
  route.reserve(reversed.size());
  for (size_t i = reversed.size(); i-- > 0;) {
    Vec3 corner = reversed[i];
    if (kStep > kCellSize / kResolution) {
      constexpr f32 kSample = kCellSize / kResolution;
      const int steps = static_cast<int>(kStep * 0.5f / kSample);
      f32 best = At(corner.x, corner.y);
      for (int dy = -steps; dy <= steps; ++dy) {
        for (int dx = -steps; dx <= steps; ++dx) {
          const f32 sx = corner.x + static_cast<f32>(dx) * kSample;
          const f32 sy = corner.y + static_cast<f32>(dy) * kSample;
          if (const f32 here = At(sx, sy); here > best) {
            best = here;
            corner = {sx, sy, 0};
          }
        }
      }
    }
    route.push_back(corner);
  }
  return route;
}

}  // namespace rx::world
