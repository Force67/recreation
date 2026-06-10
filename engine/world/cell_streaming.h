#ifndef RECREATION_WORLD_CELL_STREAMING_H_
#define RECREATION_WORLD_CELL_STREAMING_H_

#include <unordered_map>
#include <vector>

#include "asset/asset_database.h"
#include "bethesda/load_order.h"
#include "ecs/world.h"

namespace rec::world {

// Streams cells around the player. Exterior worldspaces are a grid of CELL
// records whose REFR children become entities, interiors load as a unit.
// Conversion from records to entities happens here and nowhere else.
class CellStreamer {
 public:
  CellStreamer(const bethesda::RecordStore& records, asset::AssetDatabase& assets)
      : records_(records), assets_(assets) {}

  struct Settings {
    i32 load_radius = 5;  // cells, matches uGridsToLoad=11 at default 5
  };

  void Configure(const Settings& settings) { settings_ = settings; }

  // Called each sim tick with the player position. Loads and unloads cells
  // as the player crosses cell boundaries (4096 units per cell).
  void Update(ecs::World& world, const f32 player_position[3]);

  void LoadInterior(ecs::World& world, bethesda::GlobalFormId cell_id);

  size_t loaded_cell_count() const { return loaded_.size(); }

 private:
  struct LoadedCell {
    std::vector<ecs::Entity> entities;
  };

  void LoadCell(ecs::World& world, i16 grid_x, i16 grid_y);
  void UnloadCell(ecs::World& world, u32 key);

  const bethesda::RecordStore& records_;
  asset::AssetDatabase& assets_;
  Settings settings_;
  std::unordered_map<u32, LoadedCell> loaded_;
};

}  // namespace rec::world

#endif  // RECREATION_WORLD_CELL_STREAMING_H_
