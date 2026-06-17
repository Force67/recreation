#ifndef RECREATION_NET_ACTOR_SYNC_H_
#define RECREATION_NET_ACTOR_SYNC_H_

#include <optional>
#include <vector>

#include <base/containers/unordered_map.h>

#include "core/types.h"
#include "ecs/world.h"
#include "world/quest_world.h"

namespace rec::net {

// One NPC's authoritative transform, addressed by form id. NPCs exist on every
// peer from cell data, so movement updates the EXISTING entity (found by form
// id) rather than spawning a replica the way player snapshots do. Server is
// authoritative: whatever moves an NPC (AI, a quest, a player's shove) does so
// on the host, and the resulting transform streams to clients here.
struct ActorState {
  u64 form = 0;
  f32 pos[3] = {0, 0, 0};
  f32 rot[4] = {0, 0, 0, 1};
};

std::vector<u8> EncodeActorStates(const std::vector<ActorState>& actors);
std::optional<std::vector<ActorState>> DecodeActorStates(ByteSpan data);

// Walks every NPC entity and returns its current transform. The authoritative
// snapshot the server deltas before sending.
std::vector<ActorState> CollectActorStates(ecs::World& world);

// Server delta: returns only the NPCs whose transform changed since the last
// Build, so standing NPCs cost nothing. A form seen for the first time is
// recorded but not emitted -- clients already have its spawn transform from cell
// data, so only later movement needs to travel.
class ActorReplicator {
 public:
  std::vector<ActorState> Build(const std::vector<ActorState>& snapshot);

 private:
  base::UnorderedMap<u64, ActorState> sent_;
};

// Client: applies actor states to existing NPC entities, looked up by form id
// via the registry, feeding InterpolatedTransform so motion is smooth between
// the (unreliable, low-rate) updates. Unknown / not-yet-streamed forms are
// skipped.
void ApplyActorStates(ecs::World& world, const world::QuestWorld& registry,
                      const std::vector<ActorState>& actors, f32 lerp_duration);

}  // namespace rec::net

#endif  // RECREATION_NET_ACTOR_SYNC_H_
