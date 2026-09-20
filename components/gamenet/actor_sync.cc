#include "components/gamenet/actor_sync.h"

#include <nanobuf.h>

#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#include "components/world/components.h"
#include "net/replication.h"

namespace rx::net {
namespace {

// Fixed 37-byte little-endian record:
//   u64 form | 3xf32 pos | 4xf32 rot | u8 dead
constexpr size_t kRecordSize = 8 + 3 * 4 + 4 * 4 + 1;

void AppendU32(std::vector<u8>& out, u32 v) {
  u8 buf[4];
  nanobuf::StoreLe<u32>(buf, v);
  out.insert(out.end(), buf, buf + 4);
}
void AppendU64(std::vector<u8>& out, u64 v) {
  u8 buf[8];
  nanobuf::StoreLe<u64>(buf, v);
  out.insert(out.end(), buf, buf + 8);
}
void AppendF32(std::vector<u8>& out, f32 v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  AppendU32(out, bits);
}

std::vector<u8> EncodeRecord(const ActorState& a) {
  std::vector<u8> rec;
  rec.reserve(kRecordSize);
  AppendU64(rec, a.form);
  for (f32 v : a.pos)
    AppendF32(rec, v);
  for (f32 v : a.rot)
    AppendF32(rec, v);
  rec.push_back(a.dead ? 1 : 0);
  return rec;
}

bool DecodeRecord(const u8* data, size_t size, ActorState* out) {
  if (size != kRecordSize)
    return false;
  size_t pos = 0;
  auto f32at = [&] {
    u32 bits = nanobuf::LoadLe<u32>(data + pos);
    pos += 4;
    f32 v;
    std::memcpy(&v, &bits, 4);
    return v;
  };
  out->form = nanobuf::LoadLe<u64>(data);
  pos += 8;
  for (f32& v : out->pos)
    v = f32at();
  for (f32& v : out->rot)
    v = f32at();
  out->dead = data[pos] != 0;
  return true;
}

bool Changed(const ActorState& a, const ActorState& b) {
  if (a.dead != b.dead)
    return true;
  constexpr f32 kEps = 1e-3f;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(a.pos[i] - b.pos[i]) > kEps)
      return true;
  for (int i = 0; i < 4; ++i)
    if (std::fabs(a.rot[i] - b.rot[i]) > kEps)
      return true;
  return false;
}

}  // namespace

std::vector<u8> EncodeActorStates(const std::vector<ActorState>& actors) {
  nanobuf::Writer writer;
  writer.Begin(/*fixed_len=*/6);
  writer.PutOffsetList<ActorState>(/*slot=*/2, actors, [](nanobuf::Writer& w, const ActorState& a) {
    std::vector<u8> rec = EncodeRecord(a);
    return w.HeapBytes(rec);
  });
  return writer.TakeBuffer();
}

base::Optional<base::Vector<ActorState>> DecodeActorStates(ByteSpan data) {
  std::optional<nanobuf::View> view = nanobuf::View::Parse(data.data(), data.size());
  if (!view)
    return base::nullopt;
  std::optional<nanobuf::BytesList> records = view->BytesListAt(/*slot=*/2);
  if (!records)
    return base::nullopt;

  base::Vector<ActorState> out;
  out.reserve(records->size());
  for (size_t i = 0; i < records->size(); ++i) {
    std::optional<nanobuf::BytesView> bytes = records->Get(i);
    if (!bytes)
      return base::nullopt;
    ActorState a;
    if (!DecodeRecord(bytes->data, bytes->size, &a))
      return base::nullopt;
    out.push_back(a);
  }
  return out;
}

std::vector<ActorState> CollectActorStates(ecs::World& world) {
  std::vector<ActorState> out;
  world.Each<world::Npc, world::Transform, world::FormLink>(
      [&](ecs::Entity entity, world::Npc&, world::Transform& t, world::FormLink& link) {
        if (world.Has<world::Hidden>(entity) || world.Has<world::Deleted>(entity))
          return;
        ActorState a;
        a.form = link.form.packed();
        for (int i = 0; i < 3; ++i)
          a.pos[i] = t.position[i];
        for (int i = 0; i < 4; ++i)
          a.rot[i] = t.rotation[i];
        a.dead = world.Has<world::Dead>(entity);
        out.push_back(a);
      });
  return out;
}

std::vector<ActorState> ActorReplicator::Build(const std::vector<ActorState>& snapshot) {
  std::vector<ActorState> changed;
  for (const ActorState& a : snapshot) {
    ActorState* prev = sent_.find(a.form);
    if (!prev) {
      sent_.insert(a.form, a);
      // Clients already have the spawn transform from cell data, so a form seen
      // for the first time costs nothing to skip -- unless it is already down,
      // which cell data does not say and a late joiner would otherwise never
      // learn.
      if (a.dead)
        changed.push_back(a);
      continue;
    }
    if (Changed(*prev, a)) {
      *prev = a;
      changed.push_back(a);
    }
  }
  return changed;
}

void ApplyActorStates(ecs::World& world,
                      const world::QuestWorld& registry,
                      const base::Vector<ActorState>& actors,
                      f32 lerp_duration) {
  for (const ActorState& a : actors) {
    ecs::Entity entity = registry.Find(a.form);
    if (!world.IsAlive(entity) || world.Has<world::Hidden>(entity) ||
        world.Has<world::Deleted>(entity))
      continue;
    const world::Transform* current = world.Get<world::Transform>(entity);
    if (!current)
      continue;

    world::Transform target = *current;
    for (int i = 0; i < 3; ++i)
      target.position[i] = a.pos[i];
    for (int i = 0; i < 4; ++i)
      target.rotation[i] = a.rot[i];

    // Blend from where the entity is now to the authoritative target over one
    // update interval; TickInterpolation writes the result into Transform.
    if (InterpolatedTransform* interp = world.Get<InterpolatedTransform>(entity)) {
      interp->from = *current;
      interp->to = target;
      interp->elapsed = 0;
      interp->duration = lerp_duration;
    } else {
      world.Add(entity, InterpolatedTransform{*current, target, 0, lerp_duration});
    }
    // The gait feed rides the same interpolation: without a ReplicatedGait the
    // actor system has no speed for this body and it walks its locomotion
    // machine at idle no matter how fast the transforms say it is moving.
    if (!world.Has<ReplicatedGait>(entity))
      world.Add(entity, ReplicatedGait{});
    // The host's word on whether this actor is down. A replica never kills
    // anything itself, so this tag only ever arrives from here.
    if (a.dead)
      world.Add(entity, world::Dead{});
    else
      world.Remove<world::Dead>(entity);
  }
}

}  // namespace rx::net
