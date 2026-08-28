#ifndef RECREATION_WORLD_SAVED_SPAWNS_H_
#define RECREATION_WORLD_SAVED_SPAWNS_H_

// References a savegame created while it was played, binned by the cell they
// stand in.
//
// A save's own references (the 0xFFxxxxxx REFR/ACHR change forms: dropped
// weapons, corpses, arrows in walls, scripted critters) have no record in any
// plugin, so the streamer cannot place them the way it places an authored ref.
// There are tens of thousands of them and only a few hundred are ever in the
// load ring at once, so they are not spawned when the save is read. This bins
// them by cell instead, and the streamer places each cell's share as that cell
// comes in, under the same budget the authored references spend.
//
// References the save MOVED into a different cell ride the same bins. A record
// authors a reference in one cell and the streamer places it when that cell
// comes in, so an actor who walked into the next cell, or a chest a script sent
// somewhere else, would otherwise appear where the record puts it and vanish
// with a cell it no longer stands in. Such a reference is binned here under its
// own form id and flagged `relocated`, which is what tells the streamer the
// authoring cell must not place a second copy.

#include <base/containers/span.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::world {

struct SavedSpawn {
  bethesda::GlobalFormId handle;  // synthetic, in the created-reference slot
  bethesda::GlobalFormId base;
  f32 position[3] = {};  // Bethesda game units
  f32 rotation[3] = {};  // radians, x/y/z
  f32 scale = 1.0f;
  bool actor = false;  // an ACHR: rendered as a biped, not as a static model
  // An authored reference the save moved out of its own cell, rather than one
  // the save created. `handle` is then the record's real form id, and the cell
  // that authors it has to leave it alone (see Relocated).
  bool relocated = false;
};

class SavedSpawnIndex {
 public:
  // The save names only the worldspace for a reference standing outside, so an
  // exterior spawn is binned by the grid cell its position falls in; `cell_size`
  // is that worldspace's cell edge in game units.
  void AddExterior(bethesda::GlobalFormId worldspace, f32 cell_size, const SavedSpawn& spawn);
  void AddInterior(bethesda::GlobalFormId cell, const SavedSpawn& spawn);

  base::Span<const SavedSpawn> Exterior(bethesda::GlobalFormId worldspace, i16 x, i16 y) const;
  base::Span<const SavedSpawn> Interior(bethesda::GlobalFormId cell) const;

  // Whether a reference the records author now stands in another cell, and so
  // must not be placed by the cell that authors it. Asked once per authored
  // reference the streamer is about to spawn.
  bool Relocated(u64 ref_handle) const { return relocated_.find(ref_handle) != nullptr; }

  mem_size size() const { return count_; }
  mem_size relocated() const { return relocated_.size(); }
  bool empty() const { return count_ == 0; }
  // Cells that hold at least one, and the fullest one's share: what says what a
  // single cell coming in has to spend.
  mem_size cells() const;
  mem_size busiest_cell() const;

 private:
  using ByCell = base::UnorderedMap<u32, base::Vector<SavedSpawn>>;
  // Nested rather than keyed by a mixed hash: two worldspaces sharing a grid
  // coordinate must not share a bin.
  base::UnorderedMap<u64, ByCell> exterior_;
  base::UnorderedMap<u64, base::Vector<SavedSpawn>> interior_;
  // Packed form ids of the relocated ones, so the authored cell can ask without
  // walking its bin.
  base::UnorderedMap<u64, bool> relocated_;
  mem_size count_ = 0;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_SAVED_SPAWNS_H_
