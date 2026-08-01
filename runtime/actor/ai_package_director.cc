#include "runtime/actor/ai_package_director.h"

#include <base/algorithm.h>
#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "runtime/actor/actor_system.h"
#include "components/bethesda/record.h"
#include "core/log.h"
#include "runtime/actor/npc_director.h"
#include "components/quest/ctda.h"
#include "components/script/games/skyrim/skyrim_bindings.h"
#include "components/world/cell_streaming.h"
#include "components/world/components.h"

namespace rx {
namespace {

constexpr f32 kUnitsToMeters = 0.01428f;

Vec3 GameToEngine(const f32 p[3]) {
  return {p[0] * kUnitsToMeters, p[2] * kUnitsToMeters, -p[1] * kUnitsToMeters};
}

// The engine yaw a placed reference's authored rotation means. A Bethesda model
// faces +Y and the games count their euler angles clockwise, so the engine yaw
// (about up, with -Z forward) is the negated Z angle.
f32 RefYaw(f32 game_rot_z) { return -game_rot_z; }

// Forward/right basis for an engine yaw, with -Z forward.
Vec3 ForwardOf(f32 yaw) { return {-std::sin(yaw), 0, -std::cos(yaw)}; }
Vec3 RightOf(f32 yaw) {
  const Vec3 f = ForwardOf(yaw);
  return {-f.z, 0, f.x};
}
f32 YawOfForward(const Vec3& forward) { return std::atan2(-forward.x, -forward.z); }

f32 YawOfQuat(const f32 q[4]) { return 2.0f * std::atan2(q[1], q[3]); }

void SetYaw(f32 rot[4], f32 yaw) {
  const f32 h = yaw * 0.5f;
  rot[0] = 0;
  rot[1] = std::sin(h);
  rot[2] = 0;
  rot[3] = std::cos(h);
}

u32 FormAt(const bethesda::Subrecord* sub, size_t offset) {
  if (!sub || sub->data.size() < offset + 4) return 0;
  u32 raw = 0;
  std::memcpy(&raw, sub->data.data() + offset, 4);
  return raw;
}

// Bethesda's cart-passenger animations are furniture idles: each carries its seat
// as a baked offset on the actor's COM, authored against the cart. Which idle a
// rider gets is a property of the vehicle, not of any quest, so the seats are
// filled front to back from the cart's own clip set.
const char* kCartDriverIdle = "meshes/actors/character/animations/carttraveldriveridle.hkx";
const char* kCartPassengerIdles[] = {
    "meshes/actors/character/animations/cartprisoneraidle.hkx",
    "meshes/actors/character/animations/cartprisonerbidle.hkx",
    "meshes/actors/character/animations/cartprisonercidle.hkx",
};

}  // namespace

AiPackageDirector::AiPackageDirector(EngineContext& ctx, ActorSystem* actors, NpcDirector* npc)
    : ctx_(ctx), actors_(actors), npc_(npc) {}

bool AiPackageDirector::RefRecordPose(bethesda::GlobalFormId ref, Vec3* pos, f32* yaw) const {
  bethesda::Record record;
  if (!ctx_.records || !ctx_.records->Parse(ref, &record)) return false;
  const bethesda::Subrecord* data = record.Find(FourCc('D', 'A', 'T', 'A'));
  if (!data || data->data.size() < 24) return false;
  f32 v[6];
  std::memcpy(v, data->data.data(), 24);
  *pos = GameToEngine(v);
  *yaw = RefYaw(v[5]);
  return true;
}

u64 AiPackageDirector::AliasReference(const quest::AliasDef& alias, u16 plugin) const {
  if (!ctx_.records) return 0;
  if (alias.forced_ref_raw)
    return ctx_.records->ResolveFrom(bethesda::RawFormId{alias.forced_ref_raw}, plugin).packed();
  if (alias.unique_actor_raw) {
    const bethesda::GlobalFormId base =
        ctx_.records->ResolveFrom(bethesda::RawFormId{alias.unique_actor_raw}, plugin);
    const bethesda::GlobalFormId placed = ctx_.records->PlacedRefForBase(base);
    if (placed.plugin != 0xffff) return placed.packed();
  }
  return 0;
}

void AiPackageDirector::ArmQuest(u64 quest, u16 plugin, const quest::QuestDef& def) {
  if (!ctx_.records) return;
  const size_t first_slot = slots_.size();
  for (const quest::AliasDef& alias : def.aliases) {
    if (alias.package_raw.empty()) continue;
    const u64 actor = AliasReference(alias, plugin);
    if (actor == 0) continue;  // an alias that fills at runtime is picked up later
    Slot slot;
    slot.quest = quest;
    slot.plugin = plugin;
    slot.alias = alias.id;
    slot.name = alias.name;
    slot.actor = actor;
    for (u32 raw : alias.package_raw) {
      const bethesda::GlobalFormId id = ctx_.records->ResolveFrom(bethesda::RawFormId{raw}, plugin);
      bethesda::Record record;
      if (!ctx_.records->Parse(id, &record)) continue;
      Package pack;
      pack.def = quest::ParsePackageRecord(id.packed(), record, *ctx_.records);
      pack.editor_id = record.GetString(FourCc('E', 'D', 'I', 'D'));
      if (const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D')))
        pack.has_scripts = bethesda::ParsePackageFragments(vmad->data, &pack.scripts, &pack.frags);
      slot.packages.push_back(base::move(pack));
    }
    if (slot.packages.empty()) continue;
    Vec3 at;
    f32 at_yaw = 0;
    if (RefRecordPose(
            bethesda::GlobalFormId{static_cast<u16>(actor >> 32), static_cast<u32>(actor)}, &at,
            &at_yaw))
      RX_DEBUG("packages: {} -> ref 0x{:x} at ({:.0f}, {:.0f})", slot.name, actor, at.x, at.z);
    slots_.push_back(base::move(slot));
  }

  // Attachments: a placed object whose enable parent is one of this quest's actors
  // rides with it (Skyrim's prisoner cart is enable-parented to its horse), and
  // whoever is standing in that object's footprint rides along.
  const size_t first_tow = tows_.size();
  for (const quest::AliasDef& alias : def.aliases) {
    const u64 ref = AliasReference(alias, plugin);
    if (ref == 0) continue;
    const bethesda::GlobalFormId id{static_cast<u16>(ref >> 32), static_cast<u32>(ref)};
    // Only an object rides its enable parent. Actors are enable-parented too (a
    // whole set piece is switched on by one reference), but they walk themselves.
    const bethesda::RecordStore::StoredRecord* child = ctx_.records->Find(id);
    if (!child || child->header.type != FourCc('R', 'E', 'F', 'R')) continue;
    bethesda::Record record;
    if (!ctx_.records->Parse(id, &record)) continue;
    const u32 parent_raw = FormAt(record.Find(FourCc('X', 'E', 'S', 'P')), 0);
    if (parent_raw == 0) continue;
    const u64 parent =
        ctx_.records->ResolveFrom(bethesda::RawFormId{parent_raw}, child->winning_plugin).packed();
    bool parent_drives = false;
    for (const Slot& s : slots_)
      if (s.actor == parent) parent_drives = true;
    if (!parent_drives) continue;
    Vec3 child_pos, parent_pos;
    f32 child_yaw = 0, parent_yaw = 0;
    if (!RefRecordPose(id, &child_pos, &child_yaw)) continue;
    if (!RefRecordPose(
            bethesda::GlobalFormId{static_cast<u16>(parent >> 32), static_cast<u32>(parent)},
            &parent_pos, &parent_yaw))
      continue;
    Tow tow;
    tow.ref = ref;
    tow.parent = parent;
    const Vec3 rel = child_pos - parent_pos;
    const Vec3 f = ForwardOf(parent_yaw), r = RightOf(parent_yaw);
    tow.local = {Dot(rel, r), rel.y, Dot(rel, f)};  // right, up, forward
    tow.local_yaw = child_yaw - parent_yaw;
    tows_.push_back(tow);
    RX_INFO("packages: {} rides its enable parent 0x{:x} ({:.1f} m behind)", alias.name, parent,
            -tow.local.z);
  }

  // Riders: an actor placed inside a towed object's footprint travels with it. The
  // vehicle's own clip set seats them, front seat first.
  for (size_t ti = first_tow; ti < tows_.size(); ++ti) {
    const Tow& tow = tows_[ti];
    const bethesda::GlobalFormId ride{static_cast<u16>(tow.ref >> 32), static_cast<u32>(tow.ref)};
    Vec3 ride_pos;
    f32 ride_yaw = 0;
    if (!RefRecordPose(ride, &ride_pos, &ride_yaw)) continue;
    // Is this a cart? The seated idles belong to the vehicle's own model, so read
    // the base record's mesh rather than assuming anything about the quest.
    bethesda::Record ride_rec;
    base::String model;
    if (ctx_.records->Parse(ride, &ride_rec)) {
      const u32 base_raw = FormAt(ride_rec.Find(FourCc('N', 'A', 'M', 'E')), 0);
      const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(ride);
      bethesda::Record base_rec;
      if (base_raw &&
          ctx_.records->Parse(ctx_.records->ResolveFrom(bethesda::RawFormId{base_raw},
                                                        stored ? stored->winning_plugin : 0),
                              &base_rec))
        model = base_rec.GetString(FourCc('M', 'O', 'D', 'L'));
    }
    std::transform(model.begin(), model.end(), model.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool cart = model.find("cart") != base::String::npos;
    tows_[ti].cart = cart;

    base::Vector<base::Pair<f32, size_t>> seats;  // forward offset -> slot index
    for (size_t i = first_slot; i < slots_.size(); ++i) {
      const Slot& s = slots_[i];
      if (s.actor == tow.parent) continue;  // the animal pulling it
      Vec3 pos;
      f32 yaw = 0;
      if (!RefRecordPose(
              bethesda::GlobalFormId{static_cast<u16>(s.actor >> 32), static_cast<u32>(s.actor)},
              &pos, &yaw))
        continue;
      const Vec3 rel = pos - ride_pos;
      const f32 planar = Length(Vec3{rel.x, 0, rel.z});
      if (planar > 2.6f || std::fabs(rel.y) > 2.5f) {
        RX_DEBUG("packages: {} stands {:.1f} m from 0x{:x}, not a rider", s.name, planar, tow.ref);
        continue;
      }
      seats.push_back({Dot(rel, ForwardOf(ride_yaw)), i});
    }
    base::Sort(seats.begin(), seats.end(), [](auto& a, auto& b) { return a.first > b.first; });
    int passenger = 0;
    for (auto& [forward, index] : seats) {
      const Slot& s = slots_[index];
      Vec3 pos;
      f32 yaw = 0;
      RefRecordPose(
          bethesda::GlobalFormId{static_cast<u16>(s.actor >> 32), static_cast<u32>(s.actor)}, &pos,
          &yaw);
      Rider rider;
      rider.actor = s.actor;
      rider.ride = tow.ref;
      const Vec3 rel = pos - ride_pos;
      rider.local = {Dot(rel, RightOf(ride_yaw)), rel.y, Dot(rel, ForwardOf(ride_yaw))};
      rider.local_yaw = yaw - ride_yaw;
      if (cart)
        rider.clip = passenger == 0 && seats.size() > 1
                         ? kCartDriverIdle
                         : kCartPassengerIdles[(passenger - 1 + 3) % 3];
      ++passenger;
      ++tows_[ti].seats;
      riders_.push_back(base::move(rider));
    }
    if (!seats.empty())
      RX_INFO("packages: {} rider(s) travel on 0x{:x}{}", seats.size(), tow.ref,
              cart ? " (seated on its cart idles)" : "");
  }
}

bool AiPackageDirector::ResolveDestination(const Slot& slot, const Package& pack, Vec3* out,
                                           f32* radius) const {
  if (!ctx_.records) return false;
  const quest::PackageTarget& target = pack.def.target;
  *radius = base::Max(target.radius * kUnitsToMeters, 1.6f);
  auto pose_of = [&](u64 handle) {
    Vec3 pos;
    f32 yaw = 0;
    const bool ok = RefRecordPose(
        bethesda::GlobalFormId{static_cast<u16>(handle >> 32), static_cast<u32>(handle)}, &pos,
        &yaw);
    if (ok) *out = pos;
    return ok;
  };
  switch (target.kind) {
    case quest::PackageTarget::Kind::kReference:
      return pose_of(target.ref);
    case quest::PackageTarget::Kind::kLinkedRef: {
      // Walk the actor's own linked reference, which is how a patrol chains markers.
      const bethesda::GlobalFormId self{static_cast<u16>(slot.actor >> 32),
                                        static_cast<u32>(slot.actor)};
      bethesda::Record record;
      if (!ctx_.records->Parse(self, &record)) return false;
      const u32 linked = FormAt(record.Find(FourCc('X', 'L', 'K', 'R')), 4);
      if (!linked) return false;
      const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(self);
      return pose_of(ctx_.records
                         ->ResolveFrom(bethesda::RawFormId{linked},
                                       stored ? stored->winning_plugin : slot.plugin)
                         .packed());
    }
    default:
      return false;  // hold-in-place packages have nowhere to go, which is the point
  }
}

bool AiPackageDirector::ActorPose(u64 handle, Vec3* pos, f32* yaw) const {
  if (!ctx_.quest_world) return false;
  const ecs::Entity e = ctx_.quest_world->Find(handle);
  if (!ctx_.world->IsAlive(e)) return false;
  const world::Transform* t = ctx_.world->Get<world::Transform>(e);
  if (!t) return false;
  *pos = Vec3{t->position[0], t->position[1], t->position[2]};
  *yaw = YawOfQuat(t->rotation);
  return true;
}

void AiPackageDirector::FirePackageFragment(Slot& slot, Package& pack,
                                            const base::String& function) {
  if (function.empty() || !ctx_.scripts || !ctx_.bindings) return;
  auto* binds = ctx_.bindings;
  const u64 package = pack.def.handle;
  const u64 quest = slot.quest;
  if (!pack.attached && pack.has_scripts && !pack.scripts.scripts.empty()) {
    // The PF_ script lives on the package form, and its fragment reaches the quest
    // through Package.GetOwningQuest(), so register that link with it.
    bethesda::ScriptAttachment attachment = pack.scripts;
    bethesda::ResolveScriptObjectForms(&attachment, [&](u32 raw) {
      return ctx_.records->ResolveFrom(bethesda::RawFormId{raw}, slot.plugin).packed();
    });
    ctx_.scripts->AttachScripts(package, attachment);
    pack.attached = true;
  }
  RX_INFO("packages: {} fires {}.{}", slot.name, pack.editor_id, function);
  ctx_.scripts->guest().Submit([binds, package, quest, function](script::papyrus::VirtualMachine&) {
    binds->SetPackageOwningQuest(package, quest);
    binds->RunPackageFragment(package, function);
  });
}

void AiPackageDirector::SelectPackages(const QuestStateCache& quests) {
  WorldConditionContext conditions(quests, [this](u64 handle, Vec3* out) {
    f32 yaw = 0;
    if (ActorPose(handle, out, &yaw)) return true;
    return RefRecordPose(
        bethesda::GlobalFormId{static_cast<u16>(handle >> 32), static_cast<u32>(handle)}, out,
        &yaw);
  });

  for (Slot& slot : slots_) {
    if (!quests.Running(slot.quest)) {
      if (slot.active >= 0) {
        npc_->ClearGuide(slot.actor);
        slot.active = -1;
        slot.has_dest = false;
      }
      continue;
    }
    int want = slot.forced;
    if (want < 0) {
      base::Vector<quest::PackageDef> defs;
      defs.reserve(slot.packages.size());
      for (const Package& p : slot.packages) defs.push_back(p.def);
      want = quest::SelectActivePackage(defs, conditions);
    }
    if (want == slot.active) continue;

    // Changing package: the old one's on-end fragment runs (that is where a leg of
    // a journey sets the next stage) and the new one's on-begin.
    if (slot.active >= 0) {
      Package& prev = slot.packages[static_cast<size_t>(slot.active)];
      if (!slot.fired && prev.frags.on_end.valid())
        FirePackageFragment(slot, prev, prev.frags.on_end.function);
      npc_->ClearGuide(slot.actor);
    }
    slot.active = want;
    slot.arrived = false;
    slot.fired = false;
    slot.has_dest = false;
    slot.stall = 0;
    slot.last_dist = 0;
    if (want < 0) continue;
    Package& pack = slot.packages[static_cast<size_t>(want)];
    slot.has_dest = pack.def.is_travel && ResolveDestination(slot, pack, &slot.dest, &slot.radius);
    if (pack.frags.on_begin.valid()) FirePackageFragment(slot, pack, pack.frags.on_begin.function);
    RX_INFO("packages: {} runs {}{}", slot.name, pack.editor_id,
            slot.has_dest ? " (travelling)" : "");
  }
}

void AiPackageDirector::DriveTravel(f32 dt) {
  for (Slot& slot : slots_) {
    if (slot.active < 0 || !slot.has_dest || slot.arrived) continue;
    Vec3 pos;
    f32 yaw = 0;
    if (!ActorPose(slot.actor, &pos, &yaw)) continue;  // not streamed in yet
    const f32 dist = Length(Vec3{slot.dest.x - pos.x, 0, slot.dest.z - pos.z});
    if (dist <= slot.radius) {
      slot.arrived = true;
      npc_->ClearGuide(slot.actor);
      Package& pack = slot.packages[static_cast<size_t>(slot.active)];
      RX_INFO("packages: {} arrived at {}'s target", slot.name, pack.editor_id);
      if (pack.frags.on_end.valid()) {
        FirePackageFragment(slot, pack, pack.frags.on_end.function);
        slot.fired = true;
      }
      continue;
    }
    npc_->SetGuide(slot.actor, slot.dest);
    // Progress watchdog: an actor wedged on geometry (or one whose destination sits
    // in a cell that never streamed collision) would hold the whole sequence, so
    // give up on walking and slide it along the straight line instead.
    if (slot.last_dist > 0 && slot.last_dist - dist < 0.05f) {
      slot.stall += dt;
    } else {
      slot.stall = 0;
    }
    slot.last_dist = dist;
    if (slot.stall > 3.0f) {
      const ecs::Entity e = ctx_.quest_world->Find(slot.actor);
      if (world::Transform* t = ctx_.world->Get<world::Transform>(e)) {
        const Vec3 dir = Normalize(Vec3{slot.dest.x - pos.x, 0, slot.dest.z - pos.z});
        const f32 step = base::Min(2.8f * dt, dist);
        t->position[0] += dir.x * step;
        t->position[2] += dir.z * step;
        f32 ground = t->position[1];
        if (ctx_.streamer && ctx_.streamer->GroundHeight(t->position[0], t->position[2], &ground))
          t->position[1] = ground;
        SetYaw(t->rotation, std::atan2(dir.x, dir.z));
        actors_->SetNpcGait(e, 2.8f, false, 0.0f);
      }
    }
  }
}

void AiPackageDirector::CaptureRiders(Tow& tow, const Vec3& ride_pos, f32 ride_yaw) {
  for (const Slot& slot : slots_) {
    if (slot.actor == tow.parent) continue;
    // Anything running a travel package of its own is going somewhere, not riding
    // (the escort's own horses trot alongside the cart).
    if (slot.has_dest && !slot.arrived) continue;
    bool already = false;
    for (const Rider& r : riders_)
      if (r.actor == slot.actor) already = true;
    if (already) continue;
    Vec3 pos;
    f32 yaw = 0;
    if (!ActorPose(slot.actor, &pos, &yaw)) continue;
    const Vec3 rel = pos - ride_pos;
    if (Length(Vec3{rel.x, 0, rel.z}) > 2.4f || std::fabs(rel.y) > 2.5f) continue;
    Rider rider;
    rider.actor = slot.actor;
    rider.ride = tow.ref;
    rider.local = {Dot(rel, RightOf(ride_yaw)), rel.y, Dot(rel, ForwardOf(ride_yaw))};
    rider.local_yaw = yaw - ride_yaw;
    if (tow.cart)
      rider.clip = tow.seats == 0 ? kCartDriverIdle : kCartPassengerIdles[(tow.seats - 1) % 3];
    ++tow.seats;
    RX_INFO("packages: {} boards 0x{:x}", slot.name, tow.ref);
    riders_.push_back(base::move(rider));
  }
}

void AiPackageDirector::DriveTows() {
  for (Tow& tow : tows_) {
    Vec3 parent_pos;
    f32 parent_yaw = 0;
    if (!ActorPose(tow.parent, &parent_pos, &parent_yaw)) continue;
    const ecs::Entity e = ctx_.quest_world->Find(tow.ref);
    world::Transform* t = ctx_.world->Get<world::Transform>(e);
    if (!t) continue;
    // The parent is an actor, whose transform yaw follows the biped convention;
    // take its facing from the transform and rebuild the authored rig around it.
    const Vec3 f = ForwardOf(parent_yaw), r = RightOf(parent_yaw);
    const Vec3 p = parent_pos + r * tow.local.x + Vec3{0, tow.local.y, 0} + f * tow.local.z;
    t->position[0] = p.x;
    t->position[2] = p.z;
    f32 ground = p.y;
    if (ctx_.streamer && ctx_.streamer->GroundHeight(p.x, p.z, &ground))
      t->position[1] = ground;
    else
      t->position[1] = p.y;
    SetYaw(t->rotation, YawOfForward(f) + tow.local_yaw);
    if (rider_timer_ <= 0)
      CaptureRiders(tow, Vec3{t->position[0], t->position[1], t->position[2]},
                    YawOfForward(f) + tow.local_yaw);
  }
}

void AiPackageDirector::SeatRiders() {
  for (Rider& rider : riders_) {
    const ecs::Entity ride_entity = ctx_.quest_world->Find(rider.ride);
    const world::Transform* ride = ctx_.world->Get<world::Transform>(ride_entity);
    const ecs::Entity actor_entity = ctx_.quest_world->Find(rider.actor);
    world::Transform* t = ctx_.world->Get<world::Transform>(actor_entity);
    if (!ride || !t) continue;
    if (!rider.seated && !rider.clip.empty())
      rider.seated = actors_->PlayNpcClip(actor_entity, rider.clip);
    const f32 ride_yaw = YawOfQuat(ride->rotation);
    const Vec3 ride_pos{ride->position[0], ride->position[1], ride->position[2]};
    const Vec3 f = ForwardOf(ride_yaw), r = RightOf(ride_yaw);
    // A seated clip carries its own place on the vehicle (the seat is baked into the
    // COM track), so a rider it seats is planted on the vehicle origin and the clip
    // does the rest; one without a clip keeps the spot it was authored in.
    const Vec3 local = rider.seated ? Vec3{0, 0, 0} : rider.local;
    const Vec3 p = ride_pos + r * local.x + Vec3{0, local.y, 0} + f * local.z;
    t->position[0] = p.x;
    t->position[1] = p.y;
    t->position[2] = p.z;
    const f32 yaw = YawOfForward(f) + (rider.seated ? 0.0f : rider.local_yaw);
    SetYaw(t->rotation, yaw);
    actors_->SetNpcGait(actor_entity, 0.0f, true, yaw);
  }
}

void AiPackageDirector::RunScenePackage(u64 actor, u64 package, u64 quest, u16 plugin) {
  for (Slot& slot : slots_) {
    if (slot.actor != actor) continue;
    for (size_t i = 0; i < slot.packages.size(); ++i) {
      if (slot.packages[i].def.handle != package) continue;
      slot.forced = static_cast<int>(i);
      slot.forced_handle = package;
      return;
    }
    // A scene can hand an actor a package that is not on its alias stack; add it so
    // the same execution path runs it.
    if (!ctx_.records) return;
    const bethesda::GlobalFormId id{static_cast<u16>(package >> 32), static_cast<u32>(package)};
    bethesda::Record record;
    if (!ctx_.records->Parse(id, &record)) return;
    Package pack;
    pack.def = quest::ParsePackageRecord(package, record, *ctx_.records);
    pack.editor_id = record.GetString(FourCc('E', 'D', 'I', 'D'));
    if (const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D')))
      pack.has_scripts = bethesda::ParsePackageFragments(vmad->data, &pack.scripts, &pack.frags);
    slot.packages.push_back(base::move(pack));
    slot.forced = static_cast<int>(slot.packages.size() - 1);
    slot.forced_handle = package;
    return;
  }
  // The performer has no alias package stack at all (a scene actor that only ever
  // does what the scene tells it): give it one.
  if (!ctx_.records) return;
  const bethesda::GlobalFormId id{static_cast<u16>(package >> 32), static_cast<u32>(package)};
  bethesda::Record record;
  if (!ctx_.records->Parse(id, &record)) return;
  Slot slot;
  slot.quest = quest;
  slot.plugin = plugin;
  slot.name = "scene actor";
  slot.actor = actor;
  Package pack;
  pack.def = quest::ParsePackageRecord(package, record, *ctx_.records);
  pack.editor_id = record.GetString(FourCc('E', 'D', 'I', 'D'));
  if (const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D')))
    pack.has_scripts = bethesda::ParsePackageFragments(vmad->data, &pack.scripts, &pack.frags);
  slot.packages.push_back(base::move(pack));
  slot.forced = 0;
  slot.forced_handle = package;
  slots_.push_back(base::move(slot));
}

void AiPackageDirector::StopScenePackage(u64 actor, u64 package) {
  for (Slot& slot : slots_) {
    if (slot.actor != actor || slot.forced_handle != package) continue;
    slot.forced = -1;
    slot.forced_handle = 0;
    slot.active = -1;  // re-select from the alias stack on the next pass
    slot.has_dest = false;
    npc_->ClearGuide(slot.actor);
  }
}

bool AiPackageDirector::JourneyStart(u64 quest, Vec3* pos) const {
  // Where a quest's scripted journey begins: the placement of the first alias actor
  // it stacks a travel package on. For an opening ride that is the top of the road.
  for (const Slot& slot : slots_) {
    if (slot.quest != quest) continue;
    bool travels = false;
    for (const Package& p : slot.packages)
      if (p.def.is_travel && p.def.target.kind == quest::PackageTarget::Kind::kReference)
        travels = true;
    if (!travels) continue;
    f32 yaw = 0;
    if (RefRecordPose(
            bethesda::GlobalFormId{static_cast<u16>(slot.actor >> 32),
                                   static_cast<u32>(slot.actor)},
            pos, &yaw))
      return true;
  }
  return false;
}

int AiPackageDirector::travelling_count() const {
  int n = 0;
  for (const Slot& slot : slots_)
    if (slot.active >= 0 && slot.has_dest && !slot.arrived) ++n;
  return n;
}

base::Vector<base::String> AiPackageDirector::Report() const {
  base::Vector<base::String> out;
  for (const Slot& slot : slots_) {
    if (slot.active < 0) continue;
    const Package& pack = slot.packages[static_cast<size_t>(slot.active)];
    base::String line = slot.name + ": " + pack.editor_id;
    Vec3 pos;
    f32 yaw = 0;
    if (slot.has_dest && ActorPose(slot.actor, &pos, &yaw)) {
      char tail[64];
      std::snprintf(tail, sizeof(tail), "  %.0f m to go%s",
                    Length(Vec3{slot.dest.x - pos.x, 0, slot.dest.z - pos.z}),
                    slot.arrived ? " (arrived)" : "");
      line += tail;
    }
    out.push_back(base::move(line));
  }
  return out;
}

void AiPackageDirector::Tick(f32 dt, const QuestStateCache& quests) {
  if (slots_.empty() || !ctx_.quest_world || !ctx_.world) return;
#if RECREATION_HAS_NET
  if (ctx_.client_session) return;  // host authoritative, like the rest of NPC motion
#endif
  select_timer_ -= dt;
  if (select_timer_ <= 0) {
    select_timer_ = 0.25f;
    SelectPackages(quests);
  }
  rider_timer_ -= dt;
  DriveTravel(dt);
  DriveTows();
  if (rider_timer_ <= 0) rider_timer_ = 1.0f;
  SeatRiders();
}

}  // namespace rx
