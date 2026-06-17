#include "net/actor_sync.h"

#include <cmath>
#include <cstring>

#include <nanobuf.h>

#include "net/replication.h"
#include "world/components.h"

namespace rec::net {
namespace {

// Fixed 36-byte little-endian record: u64 form | 3xf32 pos | 4xf32 rot.
constexpr size_t kRecordSize = 8 + 3 * 4 + 4 * 4;

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
  for (f32 v : a.pos) AppendF32(rec, v);
  for (f32 v : a.rot) AppendF32(rec, v);
  return rec;
}

bool DecodeRecord(const u8* data, size_t size, ActorState* out) {
  if (size != kRecordSize) return false;
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
  for (f32& v : out->pos) v = f32at();
  for (f32& v : out->rot) v = f32at();
  return true;
}

bool Changed(const ActorState& a, const ActorState& b) {
  constexpr f32 kEps = 1e-3f;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(a.pos[i] - b.pos[i]) > kEps) return true;
  for (int i = 0; i < 4; ++i)
    if (std::fabs(a.rot[i] - b.rot[i]) > kEps) return true;
  return false;
}

}  // namespace

std::vector<u8> EncodeActorStates(const std::vector<ActorState>& actors) {
  nanobuf::Writer writer;
  writer.Begin(/*fixed_len=*/6);
  writer.PutOffsetList<ActorState>(/*slot=*/2, actors,
                                   [](nanobuf::Writer& w, const ActorState& a) {
                                     std::vector<u8> rec = EncodeRecord(a);
                                     return w.HeapBytes(rec);
                                   });
  return writer.TakeBuffer();
}

std::optional<std::vector<ActorState>> DecodeActorStates(ByteSpan data) {
  std::optional<nanobuf::View> view = nanobuf::View::Parse(data.data(), data.size());
  if (!view) return std::nullopt;
  std::optional<nanobuf::BytesList> records = view->BytesListAt(/*slot=*/2);
  if (!records) return std::nullopt;

  std::vector<ActorState> out;
  out.reserve(records->size());
  for (size_t i = 0; i < records->size(); ++i) {
    std::optional<nanobuf::BytesView> bytes = records->Get(i);
    if (!bytes) return std::nullopt;
    ActorState a;
    if (!DecodeRecord(bytes->data, bytes->size, &a)) return std::nullopt;
    out.push_back(a);
  }
  return out;
}

std::vector<ActorState> CollectActorStates(ecs::World& world) {
  std::vector<ActorState> out;
  world.Each<world::Npc, world::Transform, world::FormLink>(
      [&](ecs::Entity, world::Npc&, world::Transform& t, world::FormLink& link) {
        ActorState a;
        a.form = link.form.packed();
        for (int i = 0; i < 3; ++i) a.pos[i] = t.position[i];
        for (int i = 0; i < 4; ++i) a.rot[i] = t.rotation[i];
        out.push_back(a);
      });
  return out;
}

std::vector<ActorState> ActorReplicator::Build(const std::vector<ActorState>& snapshot) {
  std::vector<ActorState> changed;
  for (const ActorState& a : snapshot) {
    ActorState* prev = sent_.find(a.form);
    if (!prev) {
      sent_.insert(a.form, a);  // clients already have the spawn transform
      continue;
    }
    if (Changed(*prev, a)) {
      *prev = a;
      changed.push_back(a);
    }
  }
  return changed;
}

void ApplyActorStates(ecs::World& world, const world::QuestWorld& registry,
                      const std::vector<ActorState>& actors, f32 lerp_duration) {
  for (const ActorState& a : actors) {
    ecs::Entity entity = registry.Find(a.form);
    if (!world.IsAlive(entity)) continue;
    const world::Transform* current = world.Get<world::Transform>(entity);
    if (!current) continue;

    world::Transform target = *current;
    for (int i = 0; i < 3; ++i) target.position[i] = a.pos[i];
    for (int i = 0; i < 4; ++i) target.rotation[i] = a.rot[i];

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
  }
}

}  // namespace rec::net
