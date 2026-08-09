#ifndef RECREATION_WORLD_MAP_DISCOVERY_H_
#define RECREATION_WORLD_MAP_DISCOVERY_H_

// Where the player has been, at the resolution the games record it.
//
// An exterior cell is uncovered a sixteenth at a time: the savegame stores a
// 16x16 bit grid per cell, which is the world map's fog of war. An interior sits
// on no grid at all, so all it can say is whether it was entered. Both are kept
// here, keyed by worldspace, because two games can be loaded at once and their
// cell coordinates mean different places.
//
// This is a store, not a map screen. It answers "has the player been here", which
// is what a map, a fast-travel list and a location prompt are all built on top of.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::world {

class MapDiscovery {
 public:
  // Bits per cell edge, so 256 tiles and 32 bytes per cell.
  static constexpr u32 kCellSubdivisions = 16;
  static constexpr u32 kCellBitmapBytes = kCellSubdivisions * kCellSubdivisions / 8;

  // Merges (never clears) the uncovered tiles of one exterior cell. Discovery
  // only ever grows, so a save loaded over a walked-in world keeps both.
  void MarkCell(bethesda::GlobalFormId worldspace, i16 x, i16 y, const u8 bits[kCellBitmapBytes]);
  // The whole cell at once, for a caller that only knows the player stood in it.
  void MarkCell(bethesda::GlobalFormId worldspace, i16 x, i16 y);
  void MarkInterior(bethesda::GlobalFormId cell);

  bool CellVisited(bethesda::GlobalFormId worldspace, i16 x, i16 y) const;
  bool InteriorVisited(bethesda::GlobalFormId cell) const;
  // Uncovered tiles of a cell, 0..256. Zero for a cell never visited.
  u32 CellTiles(bethesda::GlobalFormId worldspace, i16 x, i16 y) const;
  // One sixteenth of a cell, `tx`/`ty` in 0..15. The savegame's 256 bits are
  // read as 16 rows of 16, low bit first; the file says nothing about which
  // corner row 0 is, so this is the store's convention and only the shape of a
  // partly explored cell rests on it.
  bool TileVisited(bethesda::GlobalFormId worldspace, i16 x, i16 y, u32 tx, u32 ty) const;

  u32 VisitedCells(bethesda::GlobalFormId worldspace) const;
  u32 VisitedCells() const;
  u32 VisitedInteriors() const { return static_cast<u32>(interiors_.size()); }
  // Worldspaces with at least one visited cell, in no particular order.
  base::Vector<bethesda::GlobalFormId> Worldspaces() const;

  // Grid bounds of what is known in a worldspace, for a map view to frame.
  // False when nothing in it has been visited.
  bool Bounds(bethesda::GlobalFormId worldspace, i16* min_x, i16* min_y, i16* max_x, i16* max_y)
      const;

  void Clear();

 private:
  struct Cell {
    u8 bits[kCellBitmapBytes] = {};
  };
  // Cells are keyed by the same packed grid coordinate the record store uses, so
  // a coordinate from the streamer and one from a savegame agree.
  static u32 GridKey(i16 x, i16 y) {
    return static_cast<u32>(static_cast<u16>(x)) << 16 | static_cast<u16>(y);
  }

  base::UnorderedMap<u64, base::UnorderedMap<u32, Cell>> worlds_;
  base::UnorderedMap<u64, bool> interiors_;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_MAP_DISCOVERY_H_
