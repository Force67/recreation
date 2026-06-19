#ifndef RECREATION_NET_REPLICATION_H_
#define RECREATION_NET_REPLICATION_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "protocol.nb.h"

#include "core/types.h"
#include "ecs/world.h"
#include "world/components.h"

namespace rec::net {

// Stable identity across machines. Local Entity values differ per peer, the
// NetworkId is what snapshots address.
struct NetworkId {
  u64 value = 0;
};

// Server-side id source. Ids are never reused within a session; 0 means
// "not replicated".
NetworkId AllocateNetworkId();

// Client-side smoothing for remotely simulated transforms. Snapshots arrive
// at a fraction of the render rate, so the visible transform blends from the
// state at the previous snapshot to the newest one over the snapshot
// interval.
struct InterpolatedTransform {
  world::Transform from;
  world::Transform to;
  f32 elapsed = 0;
  f32 duration = 0;  // 0 snaps
};

// Render gait derived for a replicated actor. Snapshots carry transforms, not
// animation state, so a walking NPC would otherwise slide across a client
// without stepping. TickInterpolation fills this from the planar velocity of
// the segment it is blending across; the engine reads it to drive the actor's
// gait. Planar means the XZ ground plane (engine up is +Y), matching the speed
// the host feeds its own locomotion.
struct ReplicatedGait {
  f32 speed = 0;        // meters per second along the ground plane
  bool moving = false;  // speed above a small idle threshold
};

// Advances every InterpolatedTransform and writes the blended result into
// the entity's Transform. Runs on clients in the pre-render stage. Where an
// entity also carries ReplicatedGait, fills it from the planar velocity of
// the active interpolation segment so the engine can animate walking actors.
void TickInterpolation(ecs::World& world, f32 dt);

// Server side: walks entities carrying NetworkId straight out of the
// archetype storage and emits nanobuf EntityState records. Delta snapshots
// carry only entities whose state changed since the previous build; full
// snapshots (keyframes) carry everything and let clients reconcile despawns
// they may have missed on the unreliable channel.
class SnapshotBuilder {
 public:
  // Fills `out` for the given tick. Returns the number of entity records
  // written. `out` is reused by the caller across ticks so the vectors keep
  // their capacity.
  u32 Build(ecs::World& world, u64 server_tick, bool full, Snapshot* out);

 private:
  struct Cached {
    EntityState state;
    u64 seen_tick = 0;
  };

  base::UnorderedMap<u64, Cached> sent_;
  // Despawns accumulate until the next full snapshot covers them, since any
  // single delta carrying them can be lost.
  base::Vector<u64> pending_despawns_;
  base::Vector<u64> scratch_removed_;
};

// Client side: maps net ids onto local entities, spawning and despawning as
// snapshots dictate, and feeds InterpolatedTransform instead of writing
// transforms directly.
class SnapshotApplier {
 public:
  // Applies a parsed snapshot. `lerp_duration` is the expected gap between
  // snapshots. Returns false when the snapshot is stale (out-of-order
  // delivery) or malformed.
  bool Apply(ecs::World& world, const SnapshotView& snapshot, f32 lerp_duration);

  // Destroys every entity this applier spawned. Used on disconnect.
  void Reset(ecs::World& world);

  u64 latest_tick() const { return latest_tick_; }
  ecs::Entity Find(u64 net_id) const;
  u32 entity_count() const { return static_cast<u32>(entities_.size()); }

 private:
  struct Replica {
    ecs::Entity entity;
    u64 seen_tick = 0;
  };

  base::UnorderedMap<u64, Replica> entities_;
  base::Vector<u64> scratch_removed_;
  u64 latest_tick_ = 0;
};

}  // namespace rec::net

#endif  // RECREATION_NET_REPLICATION_H_
