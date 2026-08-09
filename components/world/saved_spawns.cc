#include "components/world/saved_spawns.h"

#include <cmath>

#include "components/bethesda/load_order.h"

namespace rx::world {
namespace {

u32 GridKeyFor(f32 x, f32 y, f32 cell_size) {
  const i16 cell_x = static_cast<i16>(std::floor(x / cell_size));
  const i16 cell_y = static_cast<i16>(std::floor(y / cell_size));
  return bethesda::RecordStore::GridKey(cell_x, cell_y);
}

}  // namespace

void SavedSpawnIndex::AddExterior(bethesda::GlobalFormId worldspace,
                                  f32 cell_size,
                                  const SavedSpawn& spawn) {
  if (cell_size <= 0.0f)
    return;
  exterior_[worldspace.packed()][GridKeyFor(spawn.position[0], spawn.position[1], cell_size)]
      .push_back(spawn);
  ++count_;
}

void SavedSpawnIndex::AddInterior(bethesda::GlobalFormId cell, const SavedSpawn& spawn) {
  interior_[cell.packed()].push_back(spawn);
  ++count_;
}

base::Span<const SavedSpawn> SavedSpawnIndex::Exterior(bethesda::GlobalFormId worldspace,
                                                       i16 x,
                                                       i16 y) const {
  const ByCell* cells = exterior_.find(worldspace.packed());
  if (!cells)
    return base::Span<const SavedSpawn>(nullptr, 0);
  const base::Vector<SavedSpawn>* list = cells->find(bethesda::RecordStore::GridKey(x, y));
  return list ? base::Span<const SavedSpawn>(list->data(), list->size())
              : base::Span<const SavedSpawn>(nullptr, 0);
}

base::Span<const SavedSpawn> SavedSpawnIndex::Interior(bethesda::GlobalFormId cell) const {
  const base::Vector<SavedSpawn>* list = interior_.find(cell.packed());
  return list ? base::Span<const SavedSpawn>(list->data(), list->size())
              : base::Span<const SavedSpawn>(nullptr, 0);
}

mem_size SavedSpawnIndex::cells() const {
  mem_size total = interior_.size();
  exterior_.ForEach([&total](const u64&, const ByCell& cells) { total += cells.size(); });
  return total;
}

mem_size SavedSpawnIndex::busiest_cell() const {
  mem_size most = 0;
  const auto widest = [&most](const u64&, const base::Vector<SavedSpawn>& list) {
    if (list.size() > most)
      most = list.size();
  };
  interior_.ForEach(widest);
  exterior_.ForEach([&widest](const u64&, const ByCell& cells) { cells.ForEach(
      [&widest](const u32& key, const base::Vector<SavedSpawn>& list) { widest(key, list); }); });
  return most;
}

}  // namespace rx::world
