#ifndef RECREATION_WORLD_COMPONENTS_H_
#define RECREATION_WORLD_COMPONENTS_H_

#include "asset/asset_id.h"
#include "components/bethesda/actor_stats.h"
#include "components/bethesda/form_id.h"
#include "components/world/prop_streaming.h"
#include "core/types.h"
#include "scene/components.h"

namespace rx::world {

// Recreation predates rx's scene module; its Transform/Renderable/Hidden were
// field-for-field the same structs. Aliased (not redefined) so they are the
// SAME components: the engine's replication (rx::net), Host draw gathering and
// recreation's world systems all address one component id.
using Transform = scene::Transform;
using Renderable = scene::Renderable;

// Links an entity back to the record that spawned it. Scripts and the savegame
// layer address entities by form id, everything else uses Entity.
struct FormLink {
  bethesda::GlobalFormId form;
};

// Marks a replicated player entity as a body the actor system renders: the
// server adds this to every joining player's entity (its own included), and
// clients apply it to the local replica when the kPlayerAvatar message names
// the entity. `base` selects the appearance: 0 means the game's default player
// template (whatever the local player wears), a non-zero packed GlobalFormId
// instances that NPC_ record instead. On the host the component is what makes
// the character pipeline step the entity; without it a player entity is an
// invisible transform.
struct PlayerAvatar {
  bethesda::GlobalFormId base{};
};

// Replicated vitals for a player entity (the kPlayerState message). The server
// is authoritative; the engine combat system is not networked yet, so today a
// host-side mod sets them through the Player.SetHealth SDK API and the value
// rides the wire from there. `dead` mirrors the entity's Dead tag on the host.
struct PlayerVitals {
  u16 health = 0;
  u16 max_health = 0;
  bool dead = false;
};

// Logical runtime references generated for pack-in children keep the authored
// template REFR here so interaction and script lookup can parse its record.
struct SourceForm {
  bethesda::GlobalFormId form;
  u64 instance_parent = 0;
};

struct CellMembership {
  i16 grid_x = 0;
  i16 grid_y = 0;
  bool interior = false;
};

// A behavior-capable placed object. Immutable streamed props intentionally do
// not carry this component (or an entity at all); promotion to ECS is what makes
// a reference individually addressable and behavior-bearing.
struct Prop {
  bethesda::GlobalFormId base;
  u32 capabilities = kPropNone;
  f32 authored_position[3] = {};
};

struct DoorState {
  bool open = false;
  bool locked = false;
  f32 closed_rotation[4] = {0, 0, 0, 1};
};

// A streamed reference deleted by script remains alive until its owning cell
// unloads so the streamer can remove physics and persist the deletion.
struct Deleted {};

enum class PropMotion : u8 { kStatic, kKinematic, kDynamic };

// Opaque rx::physics::BodyId kept alongside behavior data without coupling all
// users of world components to the physics implementation header.
struct PropPhysics {
  u64 body = 0;
  PropMotion motion = PropMotion::kStatic;
};

// An initially-disabled pack-in has a logical root; its children are created on
// first enable and follow the root without losing their own disabled state.
struct PackInRoot {
  bool spawned = false;
  bool has_composed_transform = false;
  u8 depth = 0;
  f32 position[3] = {};
  f32 rotation[4] = {0, 0, 0, 1};
  f32 scale = 1.0f;
};
struct PackInOwner {
  u64 root = 0;
  bool independently_hidden = false;
  f32 inherited_position_offset[3] = {};
  f32 observed_root_offset[3] = {};
};

// A placed NPC reference (ACHR), tagged with its base actor (NPC_) so callers
// can distinguish actors from static refs and resolve their base data. Loaded
// from cell data on every peer, so placement needs no replication; only dynamic
// changes (move/disable/spawn) ride the quest world-command channel.
struct Npc {
  bethesda::GlobalFormId base;
};

// The level and temperament of a placed actor, resolved when it streams in from
// its NPC_ record and overridden by a resumed savegame. Aliased, not redefined:
// the record layer owns the shape (components/bethesda/actor_stats.h) and the
// ECS only carries it, so one struct answers for records, saves and scripts.
using ActorStats = bethesda::ActorStats;

// Tag marking an entity disabled (Papyrus Disable()); the render pass skips it.
using Hidden = scene::Hidden;

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

// Tag marking an actor held on a piece of furniture: the carriage driver on his
// bench, the horse in the shafts. Where it stands is the furniture's business,
// so the ambient sandbox leaves it alone instead of wandering it off the cart.
struct Seated {};

// Tag marking an entity created by a quest (PlaceAtMe), with the issuing quest,
// so quest-spawned content can be found by an ECS sweep as well as via the
// QuestWorld provenance ledger.
struct QuestSpawned {
  u64 quest = 0;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_COMPONENTS_H_
