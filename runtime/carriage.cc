#include "carriage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "actor_system.h"
#include "asset/asset_database.h"
#include "asset/mesh.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "engine_context.h"
#include "fly_camera.h"
#include "render/core/renderer.h"
#include "world/components.h"

namespace rx {
namespace {

// The Skyrim fast-travel carriage body (CartFurniture, 0x00090048). One baked
// mesh; the wheels are drawn separately at the physics wheel transforms.
constexpr const char* kCarriageMesh = "meshes/furniture/cart/cartfurnstatic01.nif";
// Bethesda Z-up game units -> engine Y-up metres, matching the actor rigs.
constexpr f32 kBethScale = 0.01428f;

Vec3 BethToEngine(f32 x, f32 y, f32 z) { return {x * kBethScale, z * kBethScale, -y * kBethScale}; }

// A flat-shaded box mesh with one material, for graybox parts.
asset::Mesh ColoredBox(render::Renderer* renderer, const char* id, Vec3 half, f32 r, f32 g, f32 b,
                       bool upload) {
  asset::Material mat;
  mat.id = asset::MakeAssetId(std::string(id) + "/mat");
  mat.base_color_factor[0] = r;
  mat.base_color_factor[1] = g;
  mat.base_color_factor[2] = b;
  mat.roughness_factor = 0.8f;
  asset::Mesh mesh = asset::MakeBox(half.x, half.y, half.z, asset::MakeAssetId(id));
  for (asset::MeshLod& lod : mesh.lods) {
    if (lod.submeshes.empty())
      lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), mat.id});
    else
      for (asset::Submesh& sm : lod.submeshes) sm.material = mat.id;
  }
  if (upload && renderer) {
    renderer->UploadMaterial(mat);
    renderer->UploadMesh(mesh);
  }
  return mesh;
}

// An X-axis cylinder (a wheel: axle along local +X, disc in the Y-Z plane), so
// GetVehicleWheel's transform (right = X, up = Y) spins it about the axle.
asset::Mesh WheelCylinder(render::Renderer* renderer, const char* id, f32 radius, f32 half_width,
                          bool upload) {
  asset::Material mat;
  mat.id = asset::MakeAssetId(std::string(id) + "/mat");
  mat.base_color_factor[0] = 0.12f;
  mat.base_color_factor[1] = 0.09f;
  mat.base_color_factor[2] = 0.07f;
  mat.roughness_factor = 0.9f;

  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId(id);
  asset::MeshLod lod;
  constexpr u32 kSeg = 20;
  auto push = [&](f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz) {
    asset::Vertex v{};
    v.position[0] = x;
    v.position[1] = y;
    v.position[2] = z;
    v.normal[0] = nx;
    v.normal[1] = ny;
    v.normal[2] = nz;
    v.tangent[0] = 0;
    v.tangent[1] = 0;
    v.tangent[2] = 1;
    v.tangent[3] = 1;
    lod.vertices.push_back(v);
  };
  // Side ring (two rings of kSeg, quads between them).
  for (u32 k = 0; k < kSeg; ++k) {
    const f32 a = 6.2831853f * static_cast<f32>(k) / kSeg;
    const f32 cy = std::cos(a), sz = std::sin(a);
    push(-half_width, radius * cy, radius * sz, 0, cy, sz);
    push(half_width, radius * cy, radius * sz, 0, cy, sz);
  }
  for (u32 k = 0; k < kSeg; ++k) {
    const u32 a0 = k * 2, a1 = ((k + 1) % kSeg) * 2;
    lod.indices.push_back(a0);
    lod.indices.push_back(a1);
    lod.indices.push_back(a0 + 1);
    lod.indices.push_back(a1);
    lod.indices.push_back(a1 + 1);
    lod.indices.push_back(a0 + 1);
  }
  // Two caps.
  for (int side = 0; side < 2; ++side) {
    const f32 x = side ? half_width : -half_width;
    const f32 nx = side ? 1.0f : -1.0f;
    const u32 center = static_cast<u32>(lod.vertices.size());
    push(x, 0, 0, nx, 0, 0);
    const u32 ring0 = static_cast<u32>(lod.vertices.size());
    for (u32 k = 0; k < kSeg; ++k) {
      const f32 a = 6.2831853f * static_cast<f32>(k) / kSeg;
      push(x, radius * std::cos(a), radius * std::sin(a), nx, 0, 0);
    }
    for (u32 k = 0; k < kSeg; ++k) {
      const u32 v0 = ring0 + k, v1 = ring0 + (k + 1) % kSeg;
      if (side) {
        lod.indices.push_back(center);
        lod.indices.push_back(v0);
        lod.indices.push_back(v1);
      } else {
        lod.indices.push_back(center);
        lod.indices.push_back(v1);
        lod.indices.push_back(v0);
      }
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), mat.id});
  mesh.bounds_radius = std::sqrt(radius * radius + half_width * half_width);
  mesh.lods.push_back(std::move(lod));
  if (upload && renderer) {
    renderer->UploadMaterial(mat);
    renderer->UploadMesh(mesh);
  }
  return mesh;
}

// Bakes a Bethesda-space carriage NIF into engine space (axis swap + metre
// scale) recentred on its bounds, so a plain world::Transform (the mirrored
// chassis pose) places it. Returns false when the mesh is unavailable.
bool BakeCarriageMesh(asset::AssetDatabase* assets, render::Renderer* renderer,
                      asset::AssetId* out_id) {
  if (!assets) return false;
  const char* path = std::getenv("RX_CARRIAGE_MESH");
  const asset::Mesh* src = assets->LoadMesh(path && path[0] ? path : kCarriageMesh);
  if (!src || src->lods.empty()) return false;
  const Vec3 c = BethToEngine(src->bounds_center[0], src->bounds_center[1], src->bounds_center[2]);
  asset::Mesh baked = *src;
  baked.id = asset::MakeAssetId("carriage/body_baked");
  baked.skinned = false;
  baked.skin = {};
  for (asset::MeshLod& lod : baked.lods) {
    for (asset::Vertex& v : lod.vertices) {
      const Vec3 p = BethToEngine(v.position[0], v.position[1], v.position[2]);
      v.position[0] = p.x - c.x;
      v.position[1] = p.y - c.y;
      v.position[2] = p.z - c.z;
      const Vec3 n = BethToEngine(v.normal[0], v.normal[1], v.normal[2]);
      v.normal[0] = n.x;
      v.normal[1] = n.y;
      v.normal[2] = n.z;
    }
  }
  baked.bounds_center[0] = baked.bounds_center[1] = baked.bounds_center[2] = 0;
  baked.bounds_radius = src->bounds_radius * kBethScale;
  if (renderer) renderer->UploadMesh(baked);
  *out_id = baked.id;
  return true;
}

world::Transform TransformAt(const Vec3& p, const f32 rot[4]) {
  world::Transform t;
  t.position[0] = p.x;
  t.position[1] = p.y;
  t.position[2] = p.z;
  t.rotation[0] = rot[0];
  t.rotation[1] = rot[1];
  t.rotation[2] = rot[2];
  t.rotation[3] = rot[3];
  return t;
}

}  // namespace

CarriageSystem::CarriageSystem(EngineContext& ctx, ActorSystem* actors)
    : ctx_(ctx), actors_(actors) {
  const char* env = std::getenv("RX_CARRIAGE");
  enabled_ = env && env[0] && env[0] != '0';
  // A synthetic form so the carriage body is pickable through the normal
  // activation path; the high plugin byte keeps it clear of real load-order ids.
  carriage_form_ = bethesda::GlobalFormId{0x00FE, 0x00CA5717};
  carriage_handle_ = carriage_form_.packed();
  label_ = "Ride carriage";
}

void CarriageSystem::Step(f32 dt) {
  if (!enabled_ || !ctx_.physics || !ctx_.physics->initialized()) return;
  if (!spawned_) {
    Vec3 origin{0, 0, 0};
    if (actors_) actors_->PlayerWorldPos(&origin);
    Spawn(origin);
    if (!spawned_) {
      enabled_ = false;  // spawn failed (no physics vehicle); don't retry every frame
      return;
    }
  }

  // Advance the horse along the loop at a trot.
  route_arc_ += trot_speed_ * dt;
  const Vec3 hp = RoutePoint(route_arc_);
  const Vec3 tangent = RouteTangent(route_arc_);
  const f32 yaw = std::atan2(tangent.x, tangent.z);
  const Vec3 horse_pos{hp.x, GroundY(hp.x, hp.z, hp.y), hp.z};

  // Drive the horse entity transform + render gait.
  if (world::Transform* t = ctx_.world->Get<world::Transform>(horse_entity_)) {
    t->position[0] = horse_pos.x;
    t->position[1] = horse_pos.y;
    t->position[2] = horse_pos.z;
    const f32 h = yaw * 0.5f;
    t->rotation[0] = 0;
    t->rotation[1] = std::sin(h);
    t->rotation[2] = 0;
    t->rotation[3] = std::cos(h);
  }
  if (horse_is_rig_ && actors_) actors_->SetNpcGait(horse_entity_, trot_speed_, true, yaw);

  // The hitch point sits behind the horse at the tongue height, so the shaft
  // pulls level. Its velocity is the finite difference of the hitch point.
  Vec3 hitch = horse_pos - tangent * horse_hitch_back_;
  hitch.y = rig_.TonguePoint(*ctx_.physics).y;
  const Vec3 hitch_vel = dt > 0 ? (hitch - prev_hitch_) * (1.0f / dt) : Vec3{};
  prev_hitch_ = hitch;

  rig_.Step(*ctx_.physics, hitch, hitch_vel, dt);
}

void CarriageSystem::SyncRender() {
  if (!spawned_ || ctx_.config->headless || !ctx_.physics) return;
  // The chassis pose is mirrored into body_entity_ by the engine's physics
  // mirror; here we place the four wheels at their live physics transforms.
  for (u32 i = 0; i < 4; ++i) {
    Vec3 wp;
    f32 wr[4];
    if (!ctx_.physics->GetVehicleWheel(rig_.vehicle(), i, &wp, wr)) continue;
    if (world::Transform* t = ctx_.world->Get<world::Transform>(wheel_entity_[i])) {
      *t = TransformAt(wp, wr);
    }
  }
}

void CarriageSystem::Spawn(const Vec3& origin) {
  render::Renderer* renderer = ctx_.renderer;
  const bool draw = !ctx_.config->headless;

  // Route: a level loop the horse walks, offset so it does not sit on the player.
  route_center_ = origin + Vec3{route_radius_, 0, 0};
  route_arc_ = 0;
  const Vec3 horse_start = RoutePoint(0);
  const Vec3 tangent = RouteTangent(0);
  const f32 yaw = std::atan2(tangent.x, tangent.z);
  const f32 ground = GroundY(horse_start.x, horse_start.z, origin.y);

  // Carriage: spawn behind the horse, a little high, and let it settle.
  world::CarriageConfig cfg;
  const Vec3 carriage_pos =
      Vec3{horse_start.x, ground + 0.6f, horse_start.z} -
      tangent * (horse_hitch_back_ + cfg.tongue_z + cfg.rest_length);
  if (!rig_.Spawn(*ctx_.physics, carriage_pos, yaw, cfg)) {
    RX_WARN("carriage: could not create the free-rolling vehicle");
    return;
  }

  // Carriage body entity: the real NIF baked to engine space when data is
  // present, else a graybox chassis box. Its pose is mirrored from the physics
  // chassis via ctx_.physics_entities.
  asset::AssetId body_mesh;
  if (!(draw && BakeCarriageMesh(ctx_.assets, renderer, &body_mesh))) {
    asset::Mesh box = ColoredBox(renderer, "carriage/body", cfg.half_extent, 0.35f, 0.22f, 0.12f,
                                 draw);
    body_mesh = box.id;
  }
  body_entity_ = ctx_.world->Create();
  f32 spawn_rot[4] = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
  ctx_.world->Add(body_entity_, TransformAt(carriage_pos, spawn_rot));
  ctx_.world->Add(body_entity_, world::Renderable{body_mesh});
  ctx_.world->Add(body_entity_, world::FormLink{carriage_form_});
  if (ctx_.physics_entities) ctx_.physics_entities->push_back({rig_.body(), body_entity_});

  // Four engine-drawn wheels at the physics wheel transforms.
  asset::Mesh wheel = WheelCylinder(renderer, "carriage/wheel", cfg.wheel_radius,
                                    0.12f, draw);
  for (u32 i = 0; i < 4; ++i) {
    wheel_entity_[i] = ctx_.world->Create();
    ctx_.world->Add(wheel_entity_[i], world::Transform{});
    ctx_.world->Add(wheel_entity_[i], world::Renderable{wheel.id});
  }

  // Horse: the creature rig when the data is present, else a graybox body.
  horse_entity_ = ecs::Entity{};
  if (actors_) {
    horse_entity_ = actors_->SpawnCreatureNpc(
        "horse", "meshes/actors/horse/animations/walkforward.hkx",
        Vec3{horse_start.x, ground, horse_start.z}, yaw);
    horse_is_rig_ = ctx_.world->IsAlive(horse_entity_);
  }
  if (!horse_is_rig_) {
    asset::Mesh body = ColoredBox(renderer, "carriage/horse_body", {0.35f, 0.7f, 1.1f}, 0.30f,
                                  0.20f, 0.14f, draw);
    asset::Mesh head = ColoredBox(renderer, "carriage/horse_head", {0.22f, 0.35f, 0.5f}, 0.30f,
                                  0.20f, 0.14f, draw);
    horse_entity_ = ctx_.world->Create();
    f32 hr[4] = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    ctx_.world->Add(horse_entity_, TransformAt(Vec3{horse_start.x, ground + 0.9f, horse_start.z},
                                               hr));
    ctx_.world->Add(horse_entity_, world::Renderable{body.id});
    // A head box parented forward+up reads the horse's facing; a child entity
    // keeps it rigid to the body transform via the scene Parent component.
    ecs::Entity head_e = ctx_.world->Create();
    ctx_.world->Add(head_e, world::Transform{.position = {0, 0.35f, 0.9f}});
    ctx_.world->Add(head_e, world::Renderable{head.id});
    ctx_.world->Add(head_e, scene::Parent{horse_entity_});
  }

  prev_hitch_ = Vec3{horse_start.x, ground, horse_start.z} - tangent * horse_hitch_back_;
  spawned_ = true;
  RX_INFO("carriage: spawned {} horse + free-rolling cart at ({:.1f}, {:.1f})",
           horse_is_rig_ ? "rig" : "graybox", carriage_pos.x, carriage_pos.z);
}

Vec3 CarriageSystem::RoutePoint(f32 arc) const {
  const f32 a = arc / route_radius_;
  return route_center_ + Vec3{std::sin(a) * route_radius_, 0, std::cos(a) * route_radius_};
}

Vec3 CarriageSystem::RouteTangent(f32 arc) const {
  const f32 a = arc / route_radius_;
  return Normalize(Vec3{std::cos(a), 0, -std::sin(a)});
}

f32 CarriageSystem::GroundY(f32 x, f32 z, f32 y_hint) const {
  if (!ctx_.physics) return y_hint;
  physics::PhysicsWorld::RayHit hit;
  if (ctx_.physics->Raycast(Vec3{x, y_hint + 5.0f, z}, Vec3{0, -1, 0}, 40.0f, &hit))
    return hit.position.y;
  return y_hint;
}

Vec3 CarriageSystem::SeatWorld() const {
  Vec3 pos;
  f32 rot[4];
  if (!rig_.Pose(*ctx_.physics, &pos, rot)) return pos;
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  return pos + Rotate(q, Vec3{0, 1.0f, -0.2f});
}

bool CarriageSystem::Activate(u64 handle) {
  if (!spawned_ || handle != carriage_handle_) return false;
  riding_ = !riding_;
  if (riding_) {
    ctx_.ride_active = true;
    RX_INFO("carriage: boarded");
  } else {
    ctx_.ride_active = false;
    // Step off beside the carriage (to its left, +X of the chassis).
    Vec3 pos;
    f32 rot[4];
    if (rig_.Pose(*ctx_.physics, &pos, rot)) {
      const Quat q{rot[0], rot[1], rot[2], rot[3]};
      const Vec3 off = pos + Rotate(q, Vec3{1.6f, 0, 0});
      if (actors_) actors_->TeleportPlayer(off.x, GroundY(off.x, off.z, pos.y), off.z);
    }
    RX_INFO("carriage: dismounted");
  }
  return true;
}

const char* CarriageSystem::Label(u64 handle) const {
  return (spawned_ && handle == carriage_handle_) ? label_.c_str() : nullptr;
}

void CarriageSystem::UpdateRide(f32 dt) {
  (void)dt;
  if (!riding_ || !ctx_.physics) return;
  const Vec3 seat = SeatWorld();
  // Pin the player to the seat (capsule + transform follow the carriage).
  if (actors_) actors_->TeleportPlayer(seat.x, seat.y - actors_->PlayerCapsuleOffset(), seat.z);
  // Frame a chase camera looking forward over the carriage.
  Vec3 pos;
  f32 rot[4];
  if (!rig_.Pose(*ctx_.physics, &pos, rot)) return;
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  const Vec3 fwd = Rotate(q, Vec3{0, 0, 1});
  const Vec3 eye = seat + Vec3{0, 1.4f, 0} - fwd * 3.2f;
  const Vec3 target = seat + fwd * 4.0f + Vec3{0, 0.6f, 0};
  ctx_.walk_eye = eye;
  ctx_.walk_target = target;
  if (ctx_.camera) {
    ctx_.camera->set_position(eye);
    const Vec3 d = Normalize(target - eye);
    ctx_.camera->set_yaw_pitch(std::atan2(d.x, -d.z),
                               std::asin(std::clamp(d.y, -1.0f, 1.0f)));
  }
}

}  // namespace rx
