#ifndef RECREATION_WORLD_GRASS_BAKER_H_
#define RECREATION_WORLD_GRASS_BAKER_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rx::world {

// Scatters grass for one exterior cell straight from the game records: the
// dominant LAND texture layer at each sample point (BTXT base under the
// ATXT/VTXT stack) picks the GRAS forms its LTEX links via GNAM, and
// instances roll on a deterministic jittered grid honoring the GRAS density,
// slope and units-from-water limits. All instances merge into one mesh per
// cell (vertices transformed on the CPU, submeshes grouped per source
// material) that uploads and unloads like the land mesh. The GRAS wave-period
// field is not wired, but the model materials are flagged for the renderer's
// vertex wind so grass sways.
class GrassBaker {
 public:
  GrassBaker(const bethesda::RecordStore& records, asset::AssetDatabase& assets)
      : records_(records), assets_(assets) {}

  // Returns the merged grass mesh for the cell, or null when nothing grows.
  // `water_height` is the cell's water level in game units (very negative
  // when the cell is dry), `density_scale` multiplies every GRAS density.
  const asset::Mesh* BuildCell(const bethesda::Record& land, u16 land_plugin, i16 grid_x,
                               i16 grid_y, f32 water_height, f32 density_scale);

  size_t total_instances() const { return total_instances_; }
  size_t total_vertices() const { return total_vertices_; }

 private:
  // One GRAS form: parsed DATA plus its converted model. Null model (record
  // missing, no MODL, conversion failed) means the form never spawns.
  struct GrassType {
    const asset::Mesh* model = nullptr;
    f32 density = 0;  // spawn probability per sample point
    f32 min_slope = 0;  // degrees from horizontal
    f32 max_slope = 90;
    f32 units_from_water = 0;
    u32 water_type = 0;
    f32 position_range = 0;
    f32 height_range = 0;
    f32 color_range = 0;
  };

  const GrassType* TypeFor(u64 gras_packed);
  const base::Vector<u64>& GrassListFor(u64 ltex_packed);  // packed GRAS ids

  const bethesda::RecordStore& records_;
  asset::AssetDatabase& assets_;
  base::UnorderedMap<u64, GrassType> types_;            // GRAS id -> parsed form
  base::UnorderedMap<u64, base::Vector<u64>> ltex_grass_;  // LTEX id -> GNAM links
  base::Vector<u64> empty_list_;
  size_t total_instances_ = 0;
  size_t total_vertices_ = 0;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_GRASS_BAKER_H_
