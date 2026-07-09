#ifndef RECREATION_WORLD_COMPONENTS_H_
#define RECREATION_WORLD_COMPONENTS_H_

#include "asset/asset_id.h"
#include "bethesda/form_id.h"
#include "core/types.h"

namespace rx::world {

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

// A placed NPC reference (ACHR), tagged with its base actor (NPC_) so callers
// can distinguish actors from static refs and resolve their base data. Loaded
// from cell data on every peer, so placement needs no replication; only dynamic
// changes (move/disable/spawn) ride the quest world-command channel.
struct Npc {
  bethesda::GlobalFormId base;
};

// Tag marking an entity disabled (Papyrus Disable()); the render pass skips it.
// A tag carries no data, its presence is the state.
struct Hidden {};

// A combatant's allegiance for the melee combat driver. Actors with different
// non-zero teams auto-engage when they come within range; team 0 is a
// non-combatant (the default, so the world is not at war by accident). Set by
// whatever knows an actor's side: a battle spawner, or faction resolution.
struct CombatTeam {
  i32 team = 0;
};

// Tag marking an actor whose health reached zero, so the combat driver skips it
// as a target and the render path can hold a downed pose. Presence is the state.
struct Dead {};

// Tag marking an entity created by a quest (PlaceAtMe), with the issuing quest,
// so quest-spawned content can be found by an ECS sweep as well as via the
// QuestWorld provenance ledger.
struct QuestSpawned {
  u64 quest = 0;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_COMPONENTS_H_
