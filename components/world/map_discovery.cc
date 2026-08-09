#include "components/world/map_discovery.h"

#include <cstring>

namespace rx::world {

void MapDiscovery::MarkCell(bethesda::GlobalFormId worldspace,
                            i16 x,
                            i16 y,
                            const u8 bits[kCellBitmapBytes]) {
  Cell& cell = *worlds_[worldspace.packed()].emplace(GridKey(x, y)).first;
  for (u32 i = 0; i < kCellBitmapBytes; ++i)
    cell.bits[i] |= bits[i];
}

void MapDiscovery::MarkCell(bethesda::GlobalFormId worldspace, i16 x, i16 y) {
  u8 all[kCellBitmapBytes];
  std::memset(all, 0xff, sizeof(all));
  MarkCell(worldspace, x, y, all);
}

void MapDiscovery::MarkInterior(bethesda::GlobalFormId cell) {
  interiors_[cell.packed()] = true;
}

bool MapDiscovery::CellVisited(bethesda::GlobalFormId worldspace, i16 x, i16 y) const {
  const auto* world = worlds_.find(worldspace.packed());
  return world && world->find(GridKey(x, y)) != nullptr;
}

bool MapDiscovery::InteriorVisited(bethesda::GlobalFormId cell) const {
  return interiors_.find(cell.packed()) != nullptr;
}

u32 MapDiscovery::CellTiles(bethesda::GlobalFormId worldspace, i16 x, i16 y) const {
  const auto* world = worlds_.find(worldspace.packed());
  if (!world)
    return 0;
  const Cell* cell = world->find(GridKey(x, y));
  if (!cell)
    return 0;
  u32 tiles = 0;
  for (u32 i = 0; i < kCellBitmapBytes; ++i)
    tiles += static_cast<u32>(__builtin_popcount(cell->bits[i]));
  return tiles;
}

u32 MapDiscovery::VisitedCells(bethesda::GlobalFormId worldspace) const {
  const auto* world = worlds_.find(worldspace.packed());
  return world ? static_cast<u32>(world->size()) : 0;
}

u32 MapDiscovery::VisitedCells() const {
  u32 total = 0;
  for (const auto& entry : worlds_)
    total += static_cast<u32>(entry.value.size());
  return total;
}

base::Vector<bethesda::GlobalFormId> MapDiscovery::Worldspaces() const {
  base::Vector<bethesda::GlobalFormId> out;
  for (const auto& entry : worlds_) {
    if (entry.value.empty())
      continue;
    out.push_back(
        bethesda::GlobalFormId{static_cast<u16>(entry.key >> 32), static_cast<u32>(entry.key)});
  }
  return out;
}

bool MapDiscovery::Bounds(bethesda::GlobalFormId worldspace,
                          i16* min_x,
                          i16* min_y,
                          i16* max_x,
                          i16* max_y) const {
  const auto* world = worlds_.find(worldspace.packed());
  if (!world || world->empty())
    return false;
  i16 lo_x = 0x7fff, lo_y = 0x7fff, hi_x = -0x8000, hi_y = -0x8000;
  for (const auto& entry : *world) {
    const i16 x = static_cast<i16>(entry.key >> 16);
    const i16 y = static_cast<i16>(entry.key & 0xffff);
    lo_x = x < lo_x ? x : lo_x;
    lo_y = y < lo_y ? y : lo_y;
    hi_x = x > hi_x ? x : hi_x;
    hi_y = y > hi_y ? y : hi_y;
  }
  *min_x = lo_x;
  *min_y = lo_y;
  *max_x = hi_x;
  *max_y = hi_y;
  return true;
}

void MapDiscovery::Clear() {
  worlds_.clear();
  interiors_.clear();
}

}  // namespace rx::world
