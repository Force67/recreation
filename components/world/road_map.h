#ifndef RECREATION_WORLD_ROAD_MAP_H_
#define RECREATION_WORLD_ROAD_MAP_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "components/bethesda/form_id.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::bethesda {
class RecordStore;
}

namespace rx::world {

// Where the roads are, read off the landscape.
//
// Skyrim does not place its roads, it paints them: a road is a dirt or path
// LTEX laid over the grass in a cell's LAND record (BTXT picks a quadrant's
// base texture, ATXT/VTXT stack layers over it with a per-vertex opacity on a
// 17x17 grid). That makes the terrain the only thing in the records that knows
// where a road runs -- a carriage's marker chain names four or five stops a
// kilometre apart and says nothing at all about what lies between them.
//
// Cells go in as they stream, and the query answers in game units, so a caller
// steering something along a road works in the same space the records do.
class RoadMap {
 public:
  // Samples per cell edge. The source grid is 33 vertices across a 4096-unit
  // cell, so this loses nothing and a road (about 500 units wide) is a good
  // handful of samples across.
  static constexpr u32 kResolution = 32;

  // Where to read landscape from when a query lands on a cell not yet decoded.
  // With a source set, the map fills itself in as it is asked about, which is
  // what lets a route search walk off into cells nothing has streamed.
  void SetSource(const bethesda::RecordStore* records, bethesda::GlobalFormId worldspace);

  // Decodes the cell's LAND texture layers into a road-weight grid. Re-adding a
  // cell replaces it. No-op when the record has no texture layers.
  void AddCell(const bethesda::RecordStore& records, u64 land_ref, i16 grid_x, i16 grid_y);
  void RemoveCell(i16 grid_x, i16 grid_y);
  bool Has(i16 grid_x, i16 grid_y) const;
  size_t cell_count() const { return cells_.size(); }

  // How much road is under a point, 0 (open country) to 1 (road surface), in
  // game units. 0 where the cell is not loaded, so a caller must treat "no
  // road" and "no data" the same: keep going the way it was.
  f32 At(f32 game_x, f32 game_y);

  // Whether an LTEX is road surface. Bethesda names them for what they are
  // ("LDirt01", "LDirtPath01", "LReachDirt01"), and the tundra road, the Reach
  // road and the snow road are all one of those.
  static bool IsRoadTexture(const bethesda::RecordStore& records, bethesda::GlobalFormId ltex);

  // The most road anywhere within `radius` of a point. What a search samples
  // with, so a stride wider than a road still sees the road it steps over.
  f32 Near(f32 game_x, f32 game_y, f32 radius);

  // The best bit of road within `radius` of a point, as somewhere to aim for.
  // Returns the point itself where nothing around it is surfaced. Routing hop
  // to hop across a long journey wants this: a hop that starts and ends on the
  // road stays on it, one that ends in a field leaves it twice.
  Vec3 NearestRoad(const Vec3& near, f32 radius);

  // A route from `from` to `to` (game units, x/y) that keeps to the roads: an
  // A* over the surface where crossing open country costs several times what
  // following a road does, so it takes the long way round if the long way is
  // surfaced. Needs a source set; decodes the cells it searches as it goes.
  //
  // `stride` is the search resolution in game units: a leg between two holds
  // wants a coarse one, a leg across a village a fine one. Returns the corners
  // of the route, `from` excluded and `to` last. Empty when no route turned up
  // inside `budget` expanded samples, which is the caller's cue to head
  // straight there.
  base::Vector<Vec3> FindRoute(const Vec3& from,
                               const Vec3& to,
                               f32 stride,
                               u32 budget = 200000);

 private:
  struct Cell {
    f32 weight[kResolution * kResolution] = {};
  };
  static u32 Key(i16 grid_x, i16 grid_y);

  // Decodes the cell a point falls in, if a source is set and it has not been
  // looked at before. Null when there is no landscape there.
  const Cell* Ensure(i16 grid_x, i16 grid_y);

  base::UnorderedMap<u32, Cell> cells_;
  base::UnorderedMap<u32, bool> looked_at_;  // cells decoded or found to have no land
  const bethesda::RecordStore* source_ = nullptr;
  bethesda::GlobalFormId worldspace_{};
  // LTEX id -> is it road, so a texture is classified once however many cells
  // and quadrants use it.
  mutable base::UnorderedMap<u64, bool> road_textures_;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_ROAD_MAP_H_
