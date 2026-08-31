// saved_spawnstest: the savegame-created reference index bins by the cell the
// streamer will ask for.

#include <cstdio>

#include "components/world/saved_spawns.h"

namespace {

using rx::f32;
using rx::i16;
using rx::u32;
using rx::bethesda::GlobalFormId;
using rx::world::SavedSpawn;
using rx::world::SavedSpawnIndex;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

SavedSpawn At(u32 local, f32 x, f32 y) {
  SavedSpawn spawn;
  spawn.handle = GlobalFormId{0xfffd, local};
  spawn.base = GlobalFormId{0, 0x1234};
  spawn.position[0] = x;
  spawn.position[1] = y;
  return spawn;
}

// Skyrim's exterior cell edge, the value the engine hands the index.
constexpr f32 kCellSize = 4096.0f;

void TestExterior() {
  std::puts("exterior binning");
  SavedSpawnIndex index;
  const GlobalFormId tamriel{0, 0x3c};
  const GlobalFormId solstheim{4, 0x1};

  index.AddExterior(tamriel, kCellSize, At(1, 0.0f, 0.0f));
  index.AddExterior(tamriel, kCellSize, At(2, 4095.0f, 4095.0f));
  index.AddExterior(tamriel, kCellSize, At(3, 4096.0f, 0.0f));
  // Negative coordinates floor towards minus infinity, not towards zero: the
  // cell left of the origin is -1, and a truncating cast would call it 0.
  index.AddExterior(tamriel, kCellSize, At(4, -1.0f, -1.0f));
  index.AddExterior(tamriel, kCellSize, At(5, -4096.0f, -4096.0f));
  // Same grid coordinate, different worldspace: must not share a bin.
  index.AddExterior(solstheim, kCellSize, At(6, 0.0f, 0.0f));

  Check("six spawns", index.size() == 6);
  Check("cell 0,0 holds two", index.Exterior(tamriel, 0, 0).size() == 2);
  Check("cell 1,0 holds one", index.Exterior(tamriel, 1, 0).size() == 1);
  Check("cell -1,-1 holds two", index.Exterior(tamriel, -1, -1).size() == 2);
  Check("cell 0,0 of the other worldspace holds one",
        index.Exterior(solstheim, 0, 0).size() == 1);
  Check("an empty cell yields nothing", index.Exterior(tamriel, 9, 9).empty());
  Check("an unknown worldspace yields nothing", index.Exterior(GlobalFormId{7, 7}, 0, 0).empty());
  Check("four cells hold something", index.cells() == 4);
  Check("the fullest holds two", index.busiest_cell() == 2);

  const base::Span<const SavedSpawn> cell = index.Exterior(tamriel, 0, 0);
  Check("insertion order kept", cell.size() == 2 && cell[0].handle.local_id == 1 &&
                                    cell[1].handle.local_id == 2);
  Check("the handle keeps the created-reference slot",
        !cell.empty() && cell[0].handle.plugin == 0xfffd);
}

void TestInterior() {
  std::puts("interior binning");
  SavedSpawnIndex index;
  const GlobalFormId inn{0, 0x16778};
  index.AddInterior(inn, At(10, 100.0f, 200.0f));
  index.AddInterior(inn, At(11, -900.0f, 4000.0f));
  Check("two spawns", index.size() == 2);
  // An interior has no grid, so position must not bin it at all.
  Check("both land in the cell", index.Interior(inn).size() == 2);
  Check("another cell yields nothing", index.Interior(GlobalFormId{0, 0x99}).empty());
  Check("one cell", index.cells() == 1);
}

// A reference the records author, which the save left in another cell: it bins
// like a created one but under its own form id, and the cell whose records
// author it has to be told to leave it alone.
void TestRelocated() {
  std::puts("relocated references");
  SavedSpawnIndex index;
  const GlobalFormId tamriel{0, 0x3c};
  const GlobalFormId house{0, 0x16778};
  SavedSpawn walked = At(0x1a2b3, 8500.0f, -300.0f);
  walked.handle = GlobalFormId{0, 0x1a2b3};  // a real record, not a created id
  walked.relocated = true;
  index.AddExterior(tamriel, kCellSize, walked);
  SavedSpawn carried = At(0x1a2b4, 0.0f, 0.0f);
  carried.handle = GlobalFormId{0, 0x1a2b4};
  carried.relocated = true;
  index.AddInterior(house, carried);

  Check("both bin", index.size() == 2 && index.relocated() == 2);
  Check("the exterior one lands in the cell it stands in",
        index.Exterior(tamriel, 2, -1).size() == 1);
  Check("the interior one lands in its cell", index.Interior(house).size() == 1);
  Check("their authoring cells are told", index.Relocated(walked.handle.packed()) &&
                                              index.Relocated(carried.handle.packed()));
  // A created reference is not relocated: nothing authors it, so no cell has to
  // be held back from placing it.
  index.AddExterior(tamriel, kCellSize, At(7, 0.0f, 0.0f));
  Check("a created reference is not held back", index.relocated() == 2 &&
                                                    !index.Relocated(GlobalFormId{0xfffd, 7}
                                                                         .packed()));
}

void TestEmpty() {
  std::puts("empty index");
  SavedSpawnIndex index;
  Check("empty", index.empty() && index.size() == 0);
  Check("no cells", index.cells() == 0 && index.busiest_cell() == 0);
  Check("lookups are safe", index.Exterior(GlobalFormId{0, 0x3c}, 0, 0).empty() &&
                                index.Interior(GlobalFormId{0, 1}).empty());
  // A worldspace with no cell size would divide by zero; the spawn is refused.
  index.AddExterior(GlobalFormId{0, 0x3c}, 0.0f, At(1, 0.0f, 0.0f));
  Check("a zero cell size is refused", index.empty());
}

}  // namespace

int main() {
  std::puts("saved_spawnstest");
  TestExterior();
  TestInterior();
  TestRelocated();
  TestEmpty();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
