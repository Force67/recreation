#include "net/replication.h"

#include <cmath>

#include <base/atomic.h>

#include "core/log.h"

namespace rx::net {
namespace {

EntityState CaptureState(ecs::World& world, ecs::Entity entity, u64 net_id,
                         const world::Transform& t) {
  EntityState state;
  state.net_id = net_id;
  state.pos_x = t.position[0];
  state.pos_y = t.position[1];
  state.pos_z = t.position[2];
  state.rot_x = t.rotation[0];
  state.rot_y = t.rotation[1];
  state.rot_z = t.rotation[2];
  state.rot_w = t.rotation[3];
  state.scale = t.scale;
  if (auto* renderable = world.Get<world::Renderable>(entity)) {
    state.mesh = renderable->mesh.hash;
  }
  if (auto* link = world.Get<world::FormLink>(entity)) {
    state.form_plugin = link->form.plugin;
    state.form_local_id = link->form.local_id;
  }
  return state;
}

world::Transform StateTransform(const EntityStateView& state) {
  world::Transform t;
  t.position[0] = state.pos_x();
  t.position[1] = state.pos_y();
  t.position[2] = state.pos_z();
  t.rotation[0] = state.rot_x();
  t.rotation[1] = state.rot_y();
  t.rotation[2] = state.rot_z();
  t.rotation[3] = state.rot_w();
  t.scale = state.scale();
  return t;
}

constexpr f32 kGaitIdleSpeed = 0.05f;  // below this an actor reads as standing

// Planar (XZ) speed of the segment a replica is blending across. This is the
// velocity the smoothed transform travels at, so it matches what the host's
// own locomotion would feed the gait. A zero-length or snapping segment reads
// as idle.
f32 SegmentPlanarSpeed(const InterpolatedTransform& interp) {
  if (interp.duration <= 0) return 0;
  const f32 dx = interp.to.position[0] - interp.from.position[0];
  const f32 dz = interp.to.position[2] - interp.from.position[2];
  return std::sqrt(dx * dx + dz * dz) / interp.duration;
}

// Lerps position and scale, nlerps the quaternion along the shorter arc.
void Blend(const world::Transform& a, const world::Transform& b, f32 alpha,
           world::Transform* out) {
  for (int i = 0; i < 3; ++i) {
    out->position[i] = a.position[i] + (b.position[i] - a.position[i]) * alpha;
  }
  out->scale = a.scale + (b.scale - a.scale) * alpha;

  f32 dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] +
            a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
  const f32 sign = dot < 0 ? -1.0f : 1.0f;
  f32 q[4];
  f32 length_sq = 0;
  for (int i = 0; i < 4; ++i) {
    q[i] = a.rotation[i] + (sign * b.rotation[i] - a.rotation[i]) * alpha;
    length_sq += q[i] * q[i];
  }
  const f32 inv_length = length_sq > 0 ? 1.0f / std::sqrt(length_sq) : 0;
  for (int i = 0; i < 4; ++i) out->rotation[i] = q[i] * inv_length;
}

}  // namespace

NetworkId AllocateNetworkId() {
  static base::Atomic<u64> next{1};
  return NetworkId{next.fetch_add(1, std::memory_order_relaxed)};
}

void TickInterpolation(ecs::World& world, f32 dt) {
  world.Each<InterpolatedTransform, world::Transform>(
      [dt](ecs::Entity, InterpolatedTransform& interp, world::Transform& t) {
        if (interp.duration <= 0) {
          t = interp.to;
          return;
        }
        interp.elapsed += dt;
        const f32 alpha = interp.elapsed / interp.duration;
        if (alpha >= 1.0f) {
          t = interp.to;
          return;
        }
        Blend(interp.from, interp.to, alpha, &t);
      });

  world.Each<InterpolatedTransform, ReplicatedGait>(
      [](ecs::Entity, InterpolatedTransform& interp, ReplicatedGait& gait) {
        gait.speed = SegmentPlanarSpeed(interp);
        gait.moving = gait.speed > kGaitIdleSpeed;
      });
}

u32 SnapshotBuilder::Build(ecs::World& world, u64 server_tick, bool full,
                           Snapshot* out) {
  out->server_tick = server_tick;
  out->full = full;
  out->entities.clear();
  out->despawned.clear();

  world.Each<NetworkId, world::Transform>(
      [&](ecs::Entity entity, NetworkId& id, world::Transform& t) {
        if (id.value == 0) return;
        EntityState state = CaptureState(world, entity, id.value, t);
        Cached* cached = sent_.find(id.value);
        if (!cached) {
          out->entities.push_back(state);
          sent_.insert(id.value, Cached{std::move(state), server_tick});
          return;
        }
        cached->seen_tick = server_tick;
        if (cached->state == state) {
          if (full) out->entities.push_back(std::move(state));
          return;
        }
        out->entities.push_back(state);
        cached->state = std::move(state);
      });

  // Anything cached but not walked this tick was destroyed.
  scratch_removed_.clear();
  for (auto cached : sent_) {
    if (cached.value.seen_tick != server_tick) {
      scratch_removed_.push_back(cached.key);
    }
  }
  for (u64 net_id : scratch_removed_) {
    sent_.erase(net_id);
    pending_despawns_.push_back(net_id);
  }

  out->despawned.reserve(pending_despawns_.size());
  for (u64 net_id : pending_despawns_) out->despawned.push_back(net_id);
  // A full snapshot lets clients reconcile absences on their own, the
  // backlog has served its purpose once one ships.
  if (full) pending_despawns_.clear();

  return static_cast<u32>(out->entities.size());
}

bool SnapshotApplier::Apply(ecs::World& world, const SnapshotView& snapshot,
                            f32 lerp_duration) {
  const u64 tick = snapshot.server_tick();
  // Snapshots ride the unreliable channel; late arrivals are dropped rather
  // than rewinding remote entities.
  if (latest_tick_ != 0 && tick <= latest_tick_) return false;

  auto entities = snapshot.entities();
  auto despawned = snapshot.despawned();
  if (!entities || !despawned) return false;
  latest_tick_ = tick;

  for (size_t i = 0; i < entities->size(); ++i) {
    std::optional<EntityStateView> state = entities->Get(i);
    if (!state) return false;
    const u64 net_id = state->net_id();
    if (net_id == 0) continue;

    Replica* replica = entities_.find(net_id);
    if (replica && !world.IsAlive(replica->entity)) {
      entities_.erase(net_id);
      replica = nullptr;
    }
    if (!replica) {
      const world::Transform t = StateTransform(*state);
      ecs::Entity entity = world.Create();
      world.Add(entity, NetworkId{net_id});
      world.Add(entity, t);
      world.Add(entity, InterpolatedTransform{t, t, 0, 0});
      world.Add(entity, ReplicatedGait{});
      if (state->mesh() != 0) {
        world.Add(entity, world::Renderable{asset::AssetId{state->mesh()}});
      }
      if (state->form_plugin() && state->form_local_id()) {
        world.Add(entity, world::FormLink{bethesda::GlobalFormId{
                              *state->form_plugin(), *state->form_local_id()}});
      }
      entities_.insert(net_id, Replica{entity, tick});
      continue;
    }

    replica->seen_tick = tick;
    ecs::Entity entity = replica->entity;
    world::Transform* current = world.Get<world::Transform>(entity);
    InterpolatedTransform* interp = world.Get<InterpolatedTransform>(entity);
    if (current && interp) {
      interp->from = *current;
      interp->to = StateTransform(*state);
      interp->elapsed = 0;
      interp->duration = lerp_duration;
    }
    if (state->mesh() != 0) {
      if (auto* renderable = world.Get<world::Renderable>(entity)) {
        renderable->mesh.hash = state->mesh();
      } else {
        world.Add(entity, world::Renderable{asset::AssetId{state->mesh()}});
      }
    }
  }

  for (size_t i = 0; i < despawned->size(); ++i) {
    const u64 net_id = (*despawned)[i];
    if (Replica* replica = entities_.find(net_id)) {
      if (world.IsAlive(replica->entity)) world.Destroy(replica->entity);
      entities_.erase(net_id);
    }
  }

  // A keyframe is authoritative about what exists: drop replicas the server
  // no longer knows, covering despawn deltas that got lost in transit.
  if (snapshot.full()) {
    scratch_removed_.clear();
    for (auto replica : entities_) {
      if (replica.value.seen_tick != tick) {
        scratch_removed_.push_back(replica.key);
      }
    }
    for (u64 net_id : scratch_removed_) {
      Replica* replica = entities_.find(net_id);
      if (replica && world.IsAlive(replica->entity)) {
        world.Destroy(replica->entity);
      }
      entities_.erase(net_id);
    }
  }
  return true;
}

void SnapshotApplier::Reset(ecs::World& world) {
  for (auto replica : entities_) {
    if (world.IsAlive(replica.value.entity)) {
      world.Destroy(replica.value.entity);
    }
  }
  entities_.clear();
  latest_tick_ = 0;
}

ecs::Entity SnapshotApplier::Find(u64 net_id) const {
  const Replica* replica = entities_.find(net_id);
  return replica ? replica->entity : ecs::kInvalidEntity;
}

}  // namespace rx::net
