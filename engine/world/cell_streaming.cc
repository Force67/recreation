#include "world/cell_streaming.h"

#include <cmath>

#include "core/log.h"
#include "world/components.h"

namespace rec::world {
namespace {

constexpr f32 kCellSize = 4096.0f;

u32 CellKey(i16 x, i16 y) {
  return static_cast<u32>(static_cast<u16>(x)) << 16 | static_cast<u16>(y);
}

}  // namespace

void CellStreamer::Update(ecs::World& world, const f32 player_position[3]) {
  i16 center_x = static_cast<i16>(std::floor(player_position[0] / kCellSize));
  i16 center_y = static_cast<i16>(std::floor(player_position[1] / kCellSize));
  i32 radius = settings_.load_radius;

  for (i32 dy = -radius; dy <= radius; ++dy) {
    for (i32 dx = -radius; dx <= radius; ++dx) {
      i16 x = static_cast<i16>(center_x + dx);
      i16 y = static_cast<i16>(center_y + dy);
      if (!loaded_.contains(CellKey(x, y))) LoadCell(world, x, y);
    }
  }

  std::vector<u32> to_unload;
  for (const auto& [key, cell] : loaded_) {
    i16 x = static_cast<i16>(key >> 16);
    i16 y = static_cast<i16>(key & 0xffff);
    if (std::abs(x - center_x) > radius || std::abs(y - center_y) > radius) {
      to_unload.push_back(key);
    }
  }
  for (u32 key : to_unload) UnloadCell(world, key);
}

void CellStreamer::LoadCell(ecs::World& world, i16 grid_x, i16 grid_y) {
  LoadedCell cell;
  // TODO: find the CELL record for this grid coordinate via the worldspace
  // index, then instantiate its REFR/ACHR children: base record decides the
  // mesh, the reference carries position/rotation/scale.
  loaded_.emplace(CellKey(grid_x, grid_y), std::move(cell));
}

void CellStreamer::UnloadCell(ecs::World& world, u32 key) {
  auto it = loaded_.find(key);
  if (it == loaded_.end()) return;
  for (ecs::Entity entity : it->second.entities) world.Destroy(entity);
  loaded_.erase(it);
}

void CellStreamer::LoadInterior(ecs::World& world, bethesda::GlobalFormId cell_id) {
  // TODO: same as LoadCell but driven by the interior cell's child group.
}

}  // namespace rec::world
