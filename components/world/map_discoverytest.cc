// map_discoverytest: the discovery store a savegame's CELL bits and the walking
// player both write into. Pure state, so no records and no world are needed.

#include <cstdio>
#include <cstring>

#include "components/world/map_discovery.h"

namespace {

using rx::i16;
using rx::u32;
using rx::u8;
using rx::bethesda::GlobalFormId;
using rx::world::MapDiscovery;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr GlobalFormId kTamriel{0, 0x0000003C};
constexpr GlobalFormId kSolstheim{4, 0x00000B79};
constexpr GlobalFormId kInn{0, 0x000133C6};

void TestCellBits() {
  MapDiscovery map;
  Check("an unvisited cell is unvisited", !map.CellVisited(kTamriel, 5, -3));
  Check("an unvisited cell has no tiles", map.CellTiles(kTamriel, 5, -3) == 0);

  // The savegame's shape: 32 bytes, one bit per sixteenth of the cell.
  u8 bits[MapDiscovery::kCellBitmapBytes] = {};
  bits[0] = 0x0f;  // four tiles
  map.MarkCell(kTamriel, 5, -3, bits);
  Check("marking a cell makes it visited", map.CellVisited(kTamriel, 5, -3));
  Check("four tiles uncovered", map.CellTiles(kTamriel, 5, -3) == 4);
  Check("a negative coordinate is its own cell", !map.CellVisited(kTamriel, 5, 3));

  // Discovery only grows: a second pass merges rather than replaces.
  u8 more[MapDiscovery::kCellBitmapBytes] = {};
  more[0] = 0x30;
  more[31] = 0xff;
  map.MarkCell(kTamriel, 5, -3, more);
  Check("bits merge instead of replacing", map.CellTiles(kTamriel, 5, -3) == 14);

  // bits[0] = 0x0f is the first four tiles of row 0; bits[31] = 0xff the last
  // eight of row 15.
  Check("the low bits of the first byte are the first tiles of the first row",
        map.TileVisited(kTamriel, 5, -3, 0, 0) && map.TileVisited(kTamriel, 5, -3, 3, 0) &&
            !map.TileVisited(kTamriel, 5, -3, 6, 0));
  Check("the last byte is the end of the last row",
        map.TileVisited(kTamriel, 5, -3, 15, 15) && map.TileVisited(kTamriel, 5, -3, 8, 15));
  Check("a tile outside the cell is not visited", !map.TileVisited(kTamriel, 5, -3, 16, 0));
  Check("an unvisited cell has no tiles set", !map.TileVisited(kTamriel, 0, 0, 0, 0));

  map.MarkCell(kTamriel, 5, -3);
  Check("marking the whole cell fills all 256 tiles", map.CellTiles(kTamriel, 5, -3) == 256);
}

void TestWorldspacesAreSeparate() {
  MapDiscovery map;
  map.MarkCell(kTamriel, 0, 0);
  map.MarkCell(kSolstheim, 0, 0);
  Check("the same coordinate in two worldspaces is two cells",
        map.VisitedCells(kTamriel) == 1 && map.VisitedCells(kSolstheim) == 1 &&
            map.VisitedCells() == 2);
  Check("both worldspaces are listed", map.Worldspaces().size() == 2);
}

void TestInteriors() {
  MapDiscovery map;
  Check("an unentered interior is unvisited", !map.InteriorVisited(kInn));
  map.MarkInterior(kInn);
  map.MarkInterior(kInn);
  Check("an entered interior is visited once", map.InteriorVisited(kInn) &&
                                                   map.VisitedInteriors() == 1);
  Check("interiors are not exterior cells", map.VisitedCells() == 0);
}

void TestBounds() {
  MapDiscovery map;
  i16 min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  Check("an empty worldspace has no bounds", !map.Bounds(kTamriel, &min_x, &min_y, &max_x, &max_y));

  map.MarkCell(kTamriel, -9, 26);
  map.MarkCell(kTamriel, 5, -3);
  map.MarkCell(kTamriel, 40, 12);
  Check("bounds span every visited cell",
        map.Bounds(kTamriel, &min_x, &min_y, &max_x, &max_y) && min_x == -9 && min_y == -3 &&
            max_x == 40 && max_y == 26);
}

}  // namespace

int main() {
  std::puts("map_discoverytest");
  TestCellBits();
  TestWorldspacesAreSeparate();
  TestInteriors();
  TestBounds();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
