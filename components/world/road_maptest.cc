// road_maptest: the roads a landscape is painted with, read back out of a LAND
// record this test authors. Lays a dirt track diagonally across a cell of grass
// and checks the map reads road on it and nothing beside it, then that a route
// across the cell follows the track instead of cutting the corner.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "components/bethesda/game_profile.h"
#include "components/bethesda/load_order.h"
#include "components/bethesda/writer.h"
#include "components/world/road_map.h"
#include "core/types.h"

using namespace rx;
using namespace rx::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr u32 kWrld = FourCc('W', 'R', 'L', 'D');
constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
constexpr u32 kLand = FourCc('L', 'A', 'N', 'D');
constexpr u32 kLtex = FourCc('L', 'T', 'E', 'X');
constexpr u32 kBtxt = FourCc('B', 'T', 'X', 'T');
constexpr u32 kAtxt = FourCc('A', 'T', 'X', 'T');
constexpr u32 kVtxt = FourCc('V', 'T', 'X', 'T');

constexpr u32 kGrassTex = 0x800;
constexpr u32 kDirtTex = 0x801;
constexpr u32 kQuadGrid = 17;

GameProfile TestProfile() {
  GameProfile profile;
  profile.game = Game::kSkyrimSe;
  profile.name = "test";
  profile.plugin_version = 1.0f;
  profile.exterior_worldspace = "TestWorld";
  return profile;
}

// BTXT/ATXT: texture form id, quadrant, layer, unused.
base::Vector<u8> TextureHeader(u32 texture, u8 quadrant, u16 layer) {
  base::Vector<u8> d(8, 0);
  std::memcpy(d.data(), &texture, 4);
  d[4] = quadrant;
  std::memcpy(d.data() + 6, &layer, 2);
  return d;
}

// VTXT: one 8-byte entry per painted point of the quadrant's 17x17 grid.
base::Vector<u8> Opacity(const base::Vector<u16>& points, const base::Vector<f32>& opacity) {
  base::Vector<u8> d(points.size() * 8, 0);
  for (size_t i = 0; i < points.size(); ++i) {
    const u16 point = points[i];
    const f32 value = opacity[i];
    std::memcpy(d.data() + i * 8, &point, 2);
    std::memcpy(d.data() + i * 8 + 4, &value, 4);
  }
  return d;
}

// A dirt track running corner to corner across the cell, three grid points
// wide, so a route over it has somewhere to be and somewhere not to be.
bool OnTrack(u32 cell_x, u32 cell_y) {
  const int spread = static_cast<int>(cell_x) - static_cast<int>(cell_y);
  return spread >= -1 && spread <= 1;
}

void WritePlugin(const GameProfile& profile, const base::String& path) {
  PluginWriter plugin(profile);
  plugin.set_author("roads").set_master(true);

  RecordBuilder grass(kLtex, RawFormId{kGrassTex});
  grass.EditorId("LFieldGrass01");
  plugin.AddRecord(grass.record());
  RecordBuilder dirt(kLtex, RawFormId{kDirtTex});
  dirt.EditorId("LDirt01");  // what Skyrim surfaces its tundra roads with
  plugin.AddRecord(dirt.record());

  RecordBuilder world(kWrld, RawFormId{0x810});
  world.EditorId("TestWorld");
  plugin.AddRecord(world.record());
  RecordBuilder cell(kCell, RawFormId{0x811});
  cell.EditorId("TestCell");
  plugin.AddRecord(cell.record());

  // One LAND at cell 0,0: grass everywhere, dirt where the track runs.
  RecordBuilder land(kLand, RawFormId{0x812});
  for (u8 quadrant = 0; quadrant < 4; ++quadrant) {
    base::Vector<u8> base_header = TextureHeader(kGrassTex, quadrant, 0);
    land.Field(kBtxt, ByteSpan(base_header.data(), base_header.size()));

    base::Vector<u16> points;
    base::Vector<f32> weights;
    for (u32 gy = 0; gy < kQuadGrid; ++gy) {
      for (u32 gx = 0; gx < kQuadGrid; ++gx) {
        // Quadrant 0 is the low corner, 1 is +x, 2 is +y, 3 is both.
        const u32 cell_gx = gx + ((quadrant & 1) ? kQuadGrid - 1 : 0);
        const u32 cell_gy = gy + ((quadrant & 2) ? kQuadGrid - 1 : 0);
        if (!OnTrack(cell_gx, cell_gy))
          continue;
        points.push_back(static_cast<u16>(gy * kQuadGrid + gx));
        weights.push_back(1.0f);
      }
    }
    if (points.empty())
      continue;
    base::Vector<u8> layer_header = TextureHeader(kDirtTex, quadrant, 0);
    land.Field(kAtxt, ByteSpan(layer_header.data(), layer_header.size()));
    base::Vector<u8> vtxt = Opacity(points, weights);
    land.Field(kVtxt, ByteSpan(vtxt.data(), vtxt.size()));
  }
  plugin.AddRecord(land.record());
  plugin.Save(path);
}

}  // namespace

int main() {
  const base::String dir = std::filesystem::temp_directory_path().string();
  const base::String path = dir + "/RoadTest.esm";
  const GameProfile profile = TestProfile();
  WritePlugin(profile, path);

  LoadOrder order;
  order.Append("RoadTest.esm");
  RecordStore store;
  Check("load the authored plugin", store.LoadAll(dir, order, profile));

  std::puts("road textures:");
  Check("dirt is road surface", world::RoadMap::IsRoadTexture(store, GlobalFormId{0, kDirtTex}));
  Check("grass is not", !world::RoadMap::IsRoadTexture(store, GlobalFormId{0, kGrassTex}));

  std::puts("what is painted where:");
  world::RoadMap roads;
  roads.AddCell(store, GlobalFormId{0, 0x812}.packed(), 0, 0);
  Check("the cell decoded", roads.Has(0, 0));
  // The track runs corner to corner, so the middle of the cell is on it and the
  // off-diagonal corners are not.
  Check("road down the middle of the track", roads.At(2048, 2048) > 0.5f);
  Check("no road out in the field", roads.At(3600, 500) < 0.1f);
  Check("no road in the other corner", roads.At(500, 3600) < 0.1f);

  std::puts("routing over it:");
  // Corner to corner along the track: the direct line is the track, so this
  // checks the search runs and stays on the surface it is meant to prefer.
  const base::Vector<Vec3> along = roads.FindRoute({300, 300, 0}, {3800, 3800, 0}, 128.0f);
  Check("a route along the track was found", !along.empty());
  bool stayed = !along.empty();
  for (const Vec3& corner : along)
    stayed = stayed && roads.At(corner.x, corner.y) > 0.4f;
  Check("every corner of it is on the track", stayed);

  // Across the track: the route has to leave the road, and is allowed to.
  const base::Vector<Vec3> across = roads.FindRoute({3600, 500, 0}, {500, 3600, 0}, 128.0f);
  Check("a route across open ground was still found", !across.empty());

  std::printf("%s\n", g_failures ? "FAILED" : "all ok");
  return g_failures ? 1 : 0;
}
