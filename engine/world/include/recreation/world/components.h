#ifndef RECREATION_WORLD_COMPONENTS_H_
#define RECREATION_WORLD_COMPONENTS_H_

#include "recreation/asset/asset_id.h"
#include "recreation/bethesda/form_id.h"
#include "recreation/core/types.h"

namespace rec::world {

struct Transform {
  f32 position[3] = {0, 0, 0};
  f32 rotation[4] = {0, 0, 0, 1};  // quaternion
  f32 scale = 1.0f;
};

struct Renderable {
  asset::AssetId mesh;
};

// Links an entity back to the record that spawned it. Scripts and the savegame
// layer address entities by form id, everything else uses Entity.
struct FormLink {
  bethesda::GlobalFormId form;
};

struct CellMembership {
  i16 grid_x = 0;
  i16 grid_y = 0;
  bool interior = false;
};

}  // namespace rec::world

#endif  // RECREATION_WORLD_COMPONENTS_H_
