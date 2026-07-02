#include "demo_scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include <base/option.h>

#include "actor_system.h"
#include "asset/materialx.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "core/math.h"
#include "world/components.h"

namespace rec {

// Config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
static base::Option<const char*> Ply{"ply", nullptr, "REC_PLY"};
static base::Option<bool> OitReverse{"oit.reverse", false, "REC_OIT_REVERSE"};
static base::Option<const char*> Mtlx{"mtlx", nullptr, "REC_MTLX"};

DemoScenes::DemoScenes(EngineContext& ctx, ActorSystem* actors)
    : ctx_(ctx),
      actors_(actors),
      world_(*ctx.world),
      scheduler_(*ctx.scheduler),
      renderer_(*ctx.renderer),
      camera_(*ctx.camera),
      physics_(*ctx.physics),
      config_(*ctx.config) {}

void DemoScenes::EmitToView(f32 dt, render::FrameView& view) {
  UpdateParticles(dt, view);
  if (gpu_particle_count_ > 0) {
    view.gpu_particle_count = gpu_particle_count_;
    view.gpu_particle_emitter = gpu_particle_emitter_;
    view.gpu_particle_mode = gpu_particle_mode_;
    view.gpu_particle_radius = gpu_particle_radius_;
    view.gpu_particle_intensity = gpu_particle_intensity_;
    if (gpu_particle_mode_ == 1) {
      // The fire lights its surroundings: a warm point light hovering in the
      // flame body, intensity flickering with layered sines (the rt lighting
      // path traces real shadows from it).
      fire_time_ += dt;
      f32 t = fire_time_;
      f32 flicker = 0.82f + 0.12f * std::sin(t * 11.7f) + 0.06f * std::sin(t * 23.3f + 1.7f) +
                    0.05f * std::sin(t * 5.1f + 0.6f);
      render::PointLight l;
      l.pos_radius[0] = gpu_particle_emitter_.x;
      l.pos_radius[1] = gpu_particle_emitter_.y + 0.55f;
      l.pos_radius[2] = gpu_particle_emitter_.z;
      l.pos_radius[3] = 14.0f;
      l.color_intensity[0] = 1.0f;
      l.color_intensity[1] = 0.55f;
      l.color_intensity[2] = 0.22f;
      l.color_intensity[3] = 9.0f * flicker;
      view.lights.push_back(l);
    }
  }
  if (fur_ball_) {
    view.fur_ball = true;
    view.fur_position = fur_position_;
  }
  if (!oit_instances_.empty()) view.oit = oit_instances_;
  if (!demo_gaussians_.empty()) view.gaussians = demo_gaussians_;
  if (!demo_lights_.empty()) view.lights = demo_lights_;
}

namespace {

// A renderable that the demo scene slowly spins, exercising the ECS update path.
struct Spin {
  f32 angle = 0;
  f32 speed = 0.9f;
};

}  // namespace

void DemoScenes::CreateWaterDemoScene() {
  // Empty map with just water and a few reflectors: the fastest loop for
  // iterating water shading without streaming a game world.
  asset::Material water_material;
  water_material.id = asset::MakeAssetId("demo/water_material");
  water_material.base_color_factor[0] = 0.08f;
  water_material.base_color_factor[1] = 0.12f;
  water_material.base_color_factor[2] = 0.16f;
  water_material.base_color_factor[3] = 0.75f;
  water_material.metallic_factor = 0;
  water_material.roughness_factor = 0.05f;
  water_material.alpha_mode = asset::AlphaMode::kBlend;
  water_material.two_sided = true;
  water_material.is_water = true;

  asset::Mesh water;
  water.id = asset::MakeAssetId("demo/water");
  water.lods.emplace_back();
  asset::MeshLod& lod = water.lods[0];
  // Subdivided grid: the gerstner displacement in mesh.vs needs vertices to
  // move. 1 m spacing resolves the shortest authored wavelength (3.7 m).
  constexpr f32 kHalf = 120.0f;
  constexpr u32 kGrid = 240;
  for (u32 gy = 0; gy <= kGrid; ++gy) {
    for (u32 gx = 0; gx <= kGrid; ++gx) {
      asset::Vertex v{};
      v.position[0] = -kHalf + 2.0f * kHalf * static_cast<f32>(gx) / kGrid;
      v.position[1] = 0;
      v.position[2] = -kHalf + 2.0f * kHalf * static_cast<f32>(gy) / kGrid;
      v.normal[1] = 1;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = v.position[0] / 8.0f;
      v.uv[1] = v.position[2] / 8.0f;
      v.color = 0xffffffff;
      lod.vertices.push_back(v);
    }
  }
  for (u32 gy = 0; gy < kGrid; ++gy) {
    for (u32 gx = 0; gx < kGrid; ++gx) {
      u32 a = gy * (kGrid + 1) + gx;
      u32 b = a + 1;
      u32 c = a + (kGrid + 1);
      u32 d = c + 1;
      for (u32 index : {a, b, c, b, d, c}) lod.indices.push_back(index);
    }
  }
  asset::Submesh submesh;
  submesh.index_count = static_cast<u32>(lod.indices.size());
  submesh.material = water_material.id;
  lod.submeshes.push_back(submesh);
  water.bounds_radius = kHalf * 1.5f;

  asset::Mesh cube = asset::MakeCube(1.0f, asset::MakeAssetId("demo/cube"));
  asset::Mesh ground = asset::MakeCube(40.0f, asset::MakeAssetId("demo/ground"));
  if (!config_.headless) {
    renderer_.UploadMaterial(water_material);
    renderer_.UploadMesh(water);
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }

  // Sea floor far below, water sheet at origin, an island of cubes. The
  // cubes are rigid bodies light enough to float: jolt buoyancy keeps them
  // bobbing on the sheet.
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -48.0f, 0}});
  world_.Add(floor, world::Renderable{ground.id});
  ecs::Entity sheet = world_.Create();
  world_.Add(sheet, world::Transform{});
  world_.Add(sheet, world::Renderable{water.id});
  physics_.AddStaticBox({0, -48.0f, 0}, {40.0f, 40.0f, 40.0f});
  physics_.set_water_height([](const Vec3&, f32* height, Vec3* flow) {
    *height = 0.0f;
    if (flow) *flow = {};
    return true;
  });
  for (int i = 0; i < 6; ++i) {
    ecs::Entity block = world_.Create();
    f32 angle = static_cast<f32>(i) * 1.047f;
    Vec3 position{std::cos(angle) * 6.0f, 2.0f + (i % 3), std::sin(angle) * 6.0f};
    world_.Add(block, world::Transform{.position = {position.x, position.y, position.z}});
    world_.Add(block, world::Renderable{cube.id});
    physics::BodyId body = physics_.AddDynamicBox(position, {1.0f, 1.0f, 1.0f}, 400.0f, {});
    if (body) ctx_.physics_entities->push_back({body, block});
  }

  // An ember fountain in front of the camera to exercise the particle path.
  particles_enabled_ = true;
  particle_emitter_ = {-7.0f, 0.8f, 0.0f};

  camera_.set_position({-14.0f, 3.0f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, -0.25f);
  REC_INFO("water demo scene");
}

void DemoScenes::CreateMaterialDemoScene() {
  // A grid of spheres sweeping the extended pbr lobes so each reads against the
  // sun and the procedural sky: clearcoat, anisotropy, sheen, and a plain
  // roughness ramp as a control. One material + mesh per sphere.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/matdemo/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -8.5f, 0}});  // top at y = -0.5
  world_.Add(floor, world::Renderable{ground.id});

  int counter = 0;
  auto spawn = [&](Vec3 pos, asset::Material mat) {
    std::string tag = "builtin/matdemo/" + std::to_string(counter++);
    mat.id = asset::MakeAssetId(tag + "/mat");
    asset::Mesh sphere = asset::MakeSphere(0.5f, 32, 48, asset::MakeAssetId(tag + "/mesh"));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) {
      renderer_.UploadMaterial(mat);
      renderer_.UploadMesh(sphere);
    }
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, world::Renderable{sphere.id});
  };

  const f32 xs[5] = {-2.8f, -1.4f, 0.0f, 1.4f, 2.8f};
  for (int i = 0; i < 5; ++i) {
    f32 t = static_cast<f32>(i) / 4.0f;
    // Row 1: clearcoat 0..1 over a dark red dielectric.
    asset::Material coat;
    coat.base_color_factor[0] = 0.5f; coat.base_color_factor[1] = 0.04f;
    coat.base_color_factor[2] = 0.04f;
    coat.roughness_factor = 0.45f;
    coat.clearcoat = t;
    coat.clearcoat_roughness = 0.05f;
    spawn({xs[i], 0.0f, 1.0f}, coat);

    // Row 2: anisotropy -1..1 over a brushed metal.
    asset::Material metal;
    metal.base_color_factor[0] = 0.95f; metal.base_color_factor[1] = 0.93f;
    metal.base_color_factor[2] = 0.88f;
    metal.metallic_factor = 1.0f;
    metal.roughness_factor = 0.35f;
    metal.anisotropy = t * 2.0f - 1.0f;
    spawn({xs[i], 0.0f, -1.2f}, metal);

    // Row 3: sheen 0..1 over a matte blue cloth.
    asset::Material cloth;
    cloth.base_color_factor[0] = 0.05f; cloth.base_color_factor[1] = 0.07f;
    cloth.base_color_factor[2] = 0.25f;
    cloth.roughness_factor = 0.9f;
    cloth.sheen_color[0] = t; cloth.sheen_color[1] = t; cloth.sheen_color[2] = t;
    cloth.sheen_roughness = 0.3f;
    spawn({xs[i], 0.0f, -3.4f}, cloth);

    // Row 0 (front): transmission 0..1 glass, refracting the rows behind it.
    asset::Material glass;
    glass.base_color_factor[0] = 0.85f; glass.base_color_factor[1] = 0.95f;
    glass.base_color_factor[2] = 0.92f;
    glass.roughness_factor = 0.05f;
    glass.transmission = t;
    glass.ior = 1.5f;
    spawn({xs[i], 0.0f, 2.2f}, glass);

    // Row 4: subsurface scattering 0..1 over pale waxy skin (moved to the back).
    asset::Material skin;
    skin.base_color_factor[0] = 0.85f; skin.base_color_factor[1] = 0.6f;
    skin.base_color_factor[2] = 0.5f;
    skin.roughness_factor = 0.55f;
    skin.subsurface = t;
    skin.subsurface_color[0] = 0.9f; skin.subsurface_color[1] = 0.2f;
    skin.subsurface_color[2] = 0.12f;
    spawn({xs[i], 0.0f, -7.2f}, skin);

    // Row 5: thin-film iridescence, film thickness sweep over a dark dielectric.
    asset::Material irid;
    irid.base_color_factor[0] = 0.04f; irid.base_color_factor[1] = 0.04f;
    irid.base_color_factor[2] = 0.05f;
    irid.roughness_factor = 0.12f;
    irid.iridescence = 1.0f;
    irid.iridescence_thickness = 250.0f + t * 700.0f;  // 250..950 nm
    spawn({xs[i], 0.0f, -5.6f}, irid);
  }

  camera_.set_position({0.0f, 1.8f, 5.4f});
  camera_.set_yaw_pitch(0.0f, -0.16f);
  camera_.speed = 4.0f;
  REC_INFO("material preview: clearcoat, anisotropy, sheen and roughness sweeps");
}

void DemoScenes::UpdateParticles(f32 dt, render::FrameView& view) {
  if (!particles_enabled_) return;
  if (dt > 0.05f) dt = 0.05f;  // clamp hitches so the fountain never explodes
  auto rnd = [&]() -> f32 {
    particle_seed_ ^= particle_seed_ << 13;
    particle_seed_ ^= particle_seed_ >> 17;
    particle_seed_ ^= particle_seed_ << 5;
    return static_cast<f32>(particle_seed_ & 0xffffffu) / 16777216.0f;
  };

  // Integrate and swap-remove the dead.
  for (size_t i = 0; i < demo_particles_.size();) {
    DemoParticle& p = demo_particles_[i];
    p.life -= dt;
    if (p.life <= 0.0f) {
      demo_particles_[i] = demo_particles_.back();
      demo_particles_.pop_back();
      continue;
    }
    p.velocity.y -= 4.0f * dt;  // gravity
    p.position.x += p.velocity.x * dt;
    p.position.y += p.velocity.y * dt;
    p.position.z += p.velocity.z * dt;
    ++i;
  }

  // Spawn an upward cone of embers at a steady rate.
  particle_spawn_accum_ += 1400.0f * dt;
  u32 spawn = static_cast<u32>(particle_spawn_accum_);
  particle_spawn_accum_ -= static_cast<f32>(spawn);
  for (u32 s = 0; s < spawn && demo_particles_.size() < 20000; ++s) {
    DemoParticle p;
    p.position = particle_emitter_;
    f32 ang = rnd() * 6.2831853f;
    f32 spread = rnd() * 1.4f;
    p.velocity = {std::cos(ang) * spread, 4.5f + rnd() * 2.0f, std::sin(ang) * spread};
    p.max_life = 1.6f + rnd() * 0.8f;
    p.life = p.max_life;
    p.size = 0.12f + rnd() * 0.10f;
    p.color = {1.0f, 0.45f + rnd() * 0.3f, 0.1f};  // warm embers
    demo_particles_.push_back(p);
  }

  // Emit live billboards into the frame view.
  view.particles.reserve(demo_particles_.size());
  for (const DemoParticle& p : demo_particles_) {
    f32 t = p.life / p.max_life;  // 1 at birth, 0 at death
    render::ParticleInstance inst;
    inst.pos[0] = p.position.x;
    inst.pos[1] = p.position.y;
    inst.pos[2] = p.position.z;
    inst.size = p.size * (1.3f - 0.3f * t);
    inst.color[0] = p.color.x;
    inst.color[1] = p.color.y;
    inst.color[2] = p.color.z;
    inst.color[3] = t * t * 0.8f;  // fade out over life
    inst.prev_pos[0] = p.position.x - p.velocity.x * dt;  // one frame back, for the motion vector
    inst.prev_pos[1] = p.position.y - p.velocity.y * dt;
    inst.prev_pos[2] = p.position.z - p.velocity.z * dt;
    view.particles.push_back(inst);
  }
}

void DemoScenes::CreateGaussianDemoScene() {
  // A colored sphere reconstructed from 3D gaussian splats: fibonacci-distributed
  // points on the surface, each an isotropic gaussian tinted by its direction.
  // Demonstrates the non-triangle primitive path projecting and blending splats.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/gsplat/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -8.0f, 0}});  // top at y = 0
  world_.Add(floor, world::Renderable{ground.id});

  // REC_PLY=<path> loads a real captured splat scene instead of the procedural
  // sphere. The renderer sorts and projects them exactly the same way.
  if (const char* ply = Ply.get()) {
    if (render::LoadGaussianPly(ply, &demo_gaussians_)) {
      camera_.set_position({0.0f, 1.0f, 4.0f});
      camera_.set_yaw_pitch(0.0f, 0.0f);
      camera_.speed = 3.0f;
      REC_INFO("gaussian splat demo: {} splats from {}", demo_gaussians_.size(), ply);
      return;
    }
    REC_WARN("gaussian splat demo: ply load failed, using the procedural sphere");
  }

  const u32 kCount = 12000;
  const f32 radius = 1.6f;
  const f32 golden = 2.39996323f;
  demo_gaussians_.reserve(kCount);
  for (u32 i = 0; i < kCount; ++i) {
    f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kCount);
    f32 y = 1.0f - 2.0f * t;
    f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    f32 phi = static_cast<f32>(i) * golden;
    Vec3 dir{std::cos(phi) * r, y, std::sin(phi) * r};
    render::GaussianInstance g;
    g.position[0] = dir.x * radius;
    g.position[1] = dir.y * radius + 1.8f;
    g.position[2] = dir.z * radius;
    g.scale[0] = g.scale[1] = g.scale[2] = 0.05f;
    g.rotation[3] = 1.0f;  // identity
    g.color[0] = dir.x * 0.5f + 0.5f;
    g.color[1] = dir.y * 0.5f + 0.5f;
    g.color[2] = dir.z * 0.5f + 0.5f;
    g.opacity = 0.9f;
    demo_gaussians_.push_back(g);
  }

  camera_.set_position({0.0f, 1.9f, 5.2f});
  camera_.set_yaw_pitch(0.0f, -0.04f);
  camera_.speed = 3.0f;
  REC_INFO("gaussian splat demo: {} splats", demo_gaussians_.size());
}

void DemoScenes::CreateLodDemoScene() {
  // A row of identical spheres receding from the camera. Each sphere carries
  // three tessellation levels; the gpu cull selects a coarser lod with distance,
  // so the near sphere is smooth and the far ones turn visibly faceted.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/loddemo/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -8.1f, 0}});  // top at y = -0.1
  world_.Add(floor, world::Renderable{ground.id});

  asset::Material mat;
  mat.id = asset::MakeAssetId("builtin/loddemo/mat");
  mat.base_color_factor[0] = 0.85f;
  mat.base_color_factor[1] = 0.5f;
  mat.base_color_factor[2] = 0.2f;
  mat.roughness_factor = 0.35f;
  mat.metallic_factor = 0.0f;
  if (!config_.headless) renderer_.UploadMaterial(mat);

  // Three spheres at increasing distance, landing on lod 0 / 1 / 2 in turn.
  const Vec3 pos[3] = {{-1.6f, 0.9f, 4.5f}, {1.5f, 0.9f, 2.0f}, {-1.3f, 0.9f, -0.5f}};
  for (int i = 0; i < 3; ++i) {
    std::string tag = "builtin/loddemo/" + std::to_string(i);
    asset::Mesh sphere = asset::MakeLodSphere(1.2f, asset::MakeAssetId(tag + "/mesh"));
    for (asset::MeshLod& lod : sphere.lods) lod.submeshes[0].material = mat.id;
    if (!config_.headless) renderer_.UploadMesh(sphere);
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {pos[i].x, pos[i].y, pos[i].z}});
    world_.Add(e, world::Renderable{sphere.id});
  }

  camera_.set_position({0.0f, 1.5f, 6.5f});
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 4.0f;
  REC_INFO("lod demo: distance-based tessellation, near smooth to far faceted");
}

void DemoScenes::CreateCornellDemoScene() {
  // The classic global-illumination test: a white room with a red left wall and
  // a green right wall, open at the top and front so the sun lights the inside.
  // With gi on, the red and green bounce onto the white floor and inner boxes;
  // with gi off the white surfaces stay neutral. Reads best under --preset low
  // (ssgi) but ddgi shows the same bleed under rt.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = 0.95f;  // matte, so the bounce reads without specular
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId white = mat("builtin/cornell/white", 0.8f, 0.8f, 0.8f);
  asset::AssetId red = mat("builtin/cornell/red", 0.8f, 0.05f, 0.05f);
  asset::AssetId green = mat("builtin/cornell/green", 0.05f, 0.8f, 0.05f);

  int counter = 0;
  auto add = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, world::Renderable{mesh.id});
  };
  auto box = [&](f32 hx, f32 hy, f32 hz) {
    return asset::MakeBox(hx, hy, hz, asset::MakeAssetId("builtin/cornell/" + std::to_string(counter++)));
  };

  add(box(2.0f, 0.1f, 2.0f), white, {0, -0.1f, 0});   // floor (top at y = 0)
  add(box(2.0f, 1.6f, 0.1f), white, {0, 1.5f, -2.0f});  // back wall
  add(box(0.1f, 1.6f, 2.0f), red, {-2.0f, 1.5f, 0});    // left wall (red)
  add(box(0.1f, 1.6f, 2.0f), green, {2.0f, 1.5f, 0});   // right wall (green)
  add(box(0.45f, 0.9f, 0.45f), white, {-0.7f, 0.9f, -0.6f});  // tall box
  add(box(0.45f, 0.45f, 0.45f), white, {0.7f, 0.45f, 0.4f});  // short box

  camera_.set_position({0.0f, 1.5f, 4.7f});
  camera_.set_yaw_pitch(0.0f, -0.12f);
  camera_.speed = 3.0f;
  REC_INFO("cornell box: gi color-bleed test (red/green walls)");
}

void DemoScenes::CreateGpuParticleDemoScene() {
  // A dense ember fountain simulated entirely on the gpu: 200k particles, far
  // past the ~20k the cpu fountain caps at, proving the compute sim runs.
  asset::Mesh ground = asset::MakeCube(12.0f, asset::MakeAssetId("builtin/gpufx/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -12.0f, 0}});  // top at y = 0
  world_.Add(floor, world::Renderable{ground.id});

  gpu_particle_count_ = 200000;
  gpu_particle_emitter_ = {0.0f, 0.1f, 0.0f};

  camera_.set_position({0.0f, 2.6f, 7.5f});
  camera_.set_yaw_pitch(0.0f, -0.18f);
  camera_.speed = 4.0f;
  REC_INFO("gpu particle demo: {} compute-simulated embers", gpu_particle_count_);
}

void DemoScenes::CreateBrickDemoScene() {
  // Parallax-occlusion showcase: a procedurally textured brick wall + floor
  // under a grazing sun, so the mortar recesses parallax and self-shadow.
  constexpr u32 kTex = 256;
  auto brick_height = [](f32 u, f32 v) -> f32 {
    // Two-course running bond with rounded mortar channels.
    f32 row = v * 8.0f;
    f32 col = u * 4.0f + (static_cast<int>(row) % 2 ? 0.5f : 0.0f);
    f32 fy = row - std::floor(row);
    f32 fx = col - std::floor(col);
    auto channel = [](f32 t, f32 w) {
      f32 d = std::min(t, 1.0f - t) / w;  // distance to the mortar line
      return std::min(d, 1.0f);
    };
    f32 h = std::min(channel(fx, 0.06f), channel(fy, 0.10f));
    h = h * h * (3.0f - 2.0f * h);  // rounded shoulder
    // Slight per-brick height variation + surface grain.
    u32 bx = static_cast<u32>(col), by = static_cast<u32>(row);
    u32 seed = bx * 374761393u + by * 668265263u;
    seed = (seed ^ (seed >> 13)) * 1274126177u;
    f32 vary = 0.85f + 0.15f * static_cast<f32>(seed & 0xffu) / 255.0f;
    return h * vary;
  };
  asset::Texture height;
  height.id = asset::MakeAssetId("builtin/bricks/height");
  height.format = asset::TextureFormat::kRgba8;
  height.width = height.height = kTex;
  height.data.resize(static_cast<size_t>(kTex) * kTex * 4);
  asset::Texture albedo = height;
  albedo.id = asset::MakeAssetId("builtin/bricks/albedo");
  albedo.is_srgb = true;
  asset::Texture normal = height;
  normal.id = asset::MakeAssetId("builtin/bricks/normal");
  for (u32 y = 0; y < kTex; ++y) {
    for (u32 x = 0; x < kTex; ++x) {
      f32 u = (x + 0.5f) / kTex, v = (y + 0.5f) / kTex;
      f32 h = brick_height(u, v);
      size_t o = (static_cast<size_t>(y) * kTex + x) * 4;
      height.data[o] = height.data[o + 1] = height.data[o + 2] =
          static_cast<u8>(h * 255.0f);
      height.data[o + 3] = 255;
      // Normal from height finite differences (tangent space, +z out).
      f32 e = 1.0f / kTex;
      f32 hx = brick_height(u + e, v) - brick_height(u - e, v);
      f32 hy = brick_height(u, v + e) - brick_height(u, v - e);
      f32 nx = -hx * 6.0f, ny = -hy * 6.0f, nz = 1.0f;
      f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
      normal.data[o] = static_cast<u8>((nx / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 1] = static_cast<u8>((ny / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 2] = static_cast<u8>((nz / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 3] = 255;
      // Brick red where raised, grey mortar in the channels.
      f32 m = h < 0.55f ? 0.0f : 1.0f;
      u32 bx = static_cast<u32>(u * 4.0f * 7919u), by = static_cast<u32>(v * 8.0f);
      u32 seed = bx * 2654435761u + by * 40503u;
      f32 tint = 0.85f + 0.15f * static_cast<f32>((seed >> 8) & 0xffu) / 255.0f;
      f32 r = m * 0.58f * tint + (1.0f - m) * 0.42f;
      f32 g = m * 0.25f * tint + (1.0f - m) * 0.40f;
      f32 b = m * 0.20f * tint + (1.0f - m) * 0.38f;
      albedo.data[o] = static_cast<u8>(r * 255.0f);
      albedo.data[o + 1] = static_cast<u8>(g * 255.0f);
      albedo.data[o + 2] = static_cast<u8>(b * 255.0f);
      albedo.data[o + 3] = 255;
    }
  }
  if (!config_.headless) {
    renderer_.UploadTexture(height);
    renderer_.UploadTexture(albedo);
    renderer_.UploadTexture(normal);
  }

  asset::Material brick;
  brick.id = asset::MakeAssetId("builtin/bricks/mat");
  brick.base_color = albedo.id;
  brick.normal = normal.id;
  brick.height = height.id;
  brick.height_scale = 0.06f;
  brick.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(brick);
  asset::Material brick_flat = brick;  // a/b: same look minus the pom march
  brick_flat.id = asset::MakeAssetId("builtin/bricks/mat_flat");
  brick_flat.height = {};
  if (!config_.headless) renderer_.UploadMaterial(brick_flat);

  // Two walls side by side (pom | flat) + a floor, sun grazing along them.
  asset::Mesh wall = asset::MakeBox(3.0f, 2.0f, 0.15f, asset::MakeAssetId("builtin/bricks/wall"));
  wall.lods[0].submeshes.push_back({0, static_cast<u32>(wall.lods[0].indices.size()), brick.id});
  asset::Mesh wall_flat =
      asset::MakeBox(3.0f, 2.0f, 0.15f, asset::MakeAssetId("builtin/bricks/wall_flat"));
  wall_flat.lods[0].submeshes.push_back(
      {0, static_cast<u32>(wall_flat.lods[0].indices.size()), brick_flat.id});
  asset::Mesh floor_mesh =
      asset::MakeBox(14.0f, 0.15f, 8.0f, asset::MakeAssetId("builtin/bricks/floor"));
  floor_mesh.lods[0].submeshes.push_back(
      {0, static_cast<u32>(floor_mesh.lods[0].indices.size()), brick.id});
  if (!config_.headless) {
    renderer_.UploadMesh(wall);
    renderer_.UploadMesh(wall_flat);
    renderer_.UploadMesh(floor_mesh);
  }
  ecs::Entity w0 = world_.Create();
  world_.Add(w0, world::Transform{.position = {-3.2f, 2.0f, -2.0f}});
  world_.Add(w0, world::Renderable{wall.id});
  ecs::Entity w1 = world_.Create();
  world_.Add(w1, world::Transform{.position = {3.2f, 2.0f, -2.0f}});
  world_.Add(w1, world::Renderable{wall_flat.id});
  ecs::Entity fl = world_.Create();
  world_.Add(fl, world::Transform{.position = {0, -0.15f, 0}});
  world_.Add(fl, world::Renderable{floor_mesh.id});

  // Grazing warm sun to pop the relief.
  renderer_.settings().sun_direction = {-0.85f, -0.18f, -0.49f};
  renderer_.settings().sun_intensity = 3.0f;
  renderer_.settings().sun_color = {1.0f, 0.85f, 0.7f};

  camera_.set_position({-0.4f, 1.7f, 2.6f});
  camera_.set_yaw_pitch(-0.25f, -0.10f);
  camera_.speed = 3.0f;
  REC_INFO("brick demo: pom wall (left) vs flat normal-mapped wall (right)");
}

void DemoScenes::CreateFireDemoScene() {
  // A campfire at dusk: stone ground, a log ring, gpu-simulated flames and
  // embers (additive hdr), and a flickering point light that the rt path
  // shadows. Exercises the whole fire stack in one shot.
  asset::Material stone;
  stone.id = asset::MakeAssetId("builtin/fire/stone");
  stone.base_color_factor[0] = 0.23f;
  stone.base_color_factor[1] = 0.22f;
  stone.base_color_factor[2] = 0.21f;
  stone.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(stone);
  asset::Material wood;
  wood.id = asset::MakeAssetId("builtin/fire/wood");
  wood.base_color_factor[0] = 0.16f;
  wood.base_color_factor[1] = 0.09f;
  wood.base_color_factor[2] = 0.05f;
  wood.roughness_factor = 0.85f;
  if (!config_.headless) renderer_.UploadMaterial(wood);

  asset::Mesh ground = asset::MakeBox(30.0f, 0.2f, 30.0f, asset::MakeAssetId("builtin/fire/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), stone.id});
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -0.2f, 0}});
  world_.Add(floor, world::Renderable{ground.id});

  // Log ring: slim boxes leaned into the fire, plus a few seat rocks so the
  // flicker light has geometry to shadow.
  asset::Mesh log = asset::MakeBox(0.9f, 0.14f, 0.14f, asset::MakeAssetId("builtin/fire/log"));
  log.lods[0].submeshes.push_back({0, static_cast<u32>(log.lods[0].indices.size()), wood.id});
  if (!config_.headless) renderer_.UploadMesh(log);
  for (int i = 0; i < 5; ++i) {
    f32 a = static_cast<f32>(i) * 1.2566f;
    Quat q = QuatFromAxisAngle({0, 1, 0}, a);
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {std::cos(a) * 0.28f, 0.10f + 0.02f * i,
                                                std::sin(a) * 0.28f},
                                   .rotation = {q.x, q.y, q.z, q.w}});
    world_.Add(e, world::Renderable{log.id});
  }
  asset::Mesh rock = asset::MakeSphere(0.45f, 16, 24, asset::MakeAssetId("builtin/fire/rock"));
  rock.lods[0].submeshes.push_back({0, static_cast<u32>(rock.lods[0].indices.size()), stone.id});
  if (!config_.headless) renderer_.UploadMesh(rock);
  const f32 rocks[4][2] = {{2.2f, 0.6f}, {-1.8f, 1.4f}, {0.6f, -2.1f}, {-2.4f, -1.2f}};
  for (auto& r : rocks) {
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {r[0], 0.25f, r[1]}});
    world_.Add(e, world::Renderable{rock.id});
  }

  // Two wind-swayed banners flanking the fire (the cloth material carries the
  // wind flag; uv.y = 0 at the pinned top edge per the sway convention).
  asset::Material cloth;
  cloth.id = asset::MakeAssetId("builtin/fire/cloth");
  cloth.base_color_factor[0] = 0.55f;
  cloth.base_color_factor[1] = 0.12f;
  cloth.base_color_factor[2] = 0.10f;
  cloth.roughness_factor = 0.85f;
  cloth.two_sided = true;
  cloth.wind = true;
  cloth.sheen_color[0] = cloth.sheen_color[1] = cloth.sheen_color[2] = 0.35f;
  cloth.sheen_roughness = 0.5f;
  if (!config_.headless) renderer_.UploadMaterial(cloth);

  asset::Mesh banner;
  banner.id = asset::MakeAssetId("builtin/fire/banner");
  banner.lods.resize(1);
  {
    constexpr int kW = 8, kH = 12;  // subdivided so the sway bends smoothly
    const f32 width = 0.9f, height = 1.6f;
    asset::MeshLod& lod = banner.lods[0];
    for (int y = 0; y <= kH; ++y) {
      for (int x = 0; x <= kW; ++x) {
        asset::Vertex v{};
        f32 u = static_cast<f32>(x) / kW;
        f32 vv = static_cast<f32>(y) / kH;
        v.position[0] = (u - 0.5f) * width;
        v.position[1] = -vv * height;  // hangs downward from the rod
        v.position[2] = 0.0f;
        v.normal[2] = 1.0f;
        v.tangent[0] = 1.0f;
        v.tangent[3] = 1.0f;
        v.uv[0] = u;
        v.uv[1] = vv;  // 0 at the pinned edge
        lod.vertices.push_back(v);
      }
    }
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        u32 a = y * (kW + 1) + x;
        u32 b = a + 1;
        u32 c = a + (kW + 1);
        u32 d = c + 1;
        for (u32 idx : {a, c, b, b, c, d}) lod.indices.push_back(idx);
      }
    }
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), cloth.id});
  }
  if (!config_.headless) renderer_.UploadMesh(banner);
  asset::Mesh pole = asset::MakeBox(0.04f, 1.3f, 0.04f, asset::MakeAssetId("builtin/fire/pole"));
  pole.lods[0].submeshes.push_back({0, static_cast<u32>(pole.lods[0].indices.size()), wood.id});
  if (!config_.headless) renderer_.UploadMesh(pole);
  const f32 banners[2][2] = {{-2.6f, -0.8f}, {2.3f, -1.6f}};
  for (auto& b : banners) {
    ecs::Entity pe = world_.Create();
    world_.Add(pe, world::Transform{.position = {b[0], 1.3f, b[1]}});
    world_.Add(pe, world::Renderable{pole.id});
    ecs::Entity be = world_.Create();
    world_.Add(be, world::Transform{.position = {b[0], 2.5f, b[1]}});
    world_.Add(be, world::Renderable{banner.id});
  }

  gpu_particle_count_ = 3000;
  gpu_particle_emitter_ = {0.0f, 0.12f, 0.0f};
  gpu_particle_mode_ = 1;
  gpu_particle_radius_ = 0.26f;
  gpu_particle_intensity_ = 0.55f;

  // Dusk: the sun barely up so the fire owns the scene. Auto exposure would
  // brighten the dim scene back to noon; bias it down for the mood.
  renderer_.settings().sun_direction = {0.35f, -0.08f, -0.93f};
  renderer_.settings().sun_intensity = 0.25f;
  renderer_.settings().sun_color = {1.0f, 0.55f, 0.35f};
  renderer_.settings().ambient = 0.03f;
  renderer_.settings().cloud_coverage = 0.6f;
  renderer_.settings().exposure = 0.30f;

  camera_.set_position({2.6f, 1.5f, 3.4f});
  camera_.set_yaw_pitch(-0.65f, -0.22f);
  camera_.speed = 3.0f;
  REC_INFO("fire demo: {} gpu flame/ember particles + flickering shadowed light",
           gpu_particle_count_);
}

void DemoScenes::CreateFurDemoScene() {
  // A fuzzy sphere: an opaque brown core (so the shells have a solid base and a
  // depth occluder) under the shell-fur pass that draws the coat.
  asset::Mesh ground = asset::MakeCube(12.0f, asset::MakeAssetId("builtin/fur/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -12.0f, 0}});  // top at y = 0
  world_.Add(floor, world::Renderable{ground.id});

  asset::Material core;
  core.id = asset::MakeAssetId("builtin/fur/core");
  core.base_color_factor[0] = 0.4f;
  core.base_color_factor[1] = 0.28f;
  core.base_color_factor[2] = 0.15f;
  core.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(core);
  asset::Mesh sphere = asset::MakeSphere(1.0f, 64, 96, asset::MakeAssetId("builtin/fur/coremesh"));
  sphere.lods[0].submeshes[0].material = core.id;
  if (!config_.headless) renderer_.UploadMesh(sphere);
  ecs::Entity ball = world_.Create();
  world_.Add(ball, world::Transform{.position = {0, 1.05f, 0}});
  world_.Add(ball, world::Renderable{sphere.id});

  fur_ball_ = true;
  fur_position_ = {0.0f, 1.05f, 0.0f};

  camera_.set_position({0.0f, 1.4f, 4.2f});
  camera_.set_yaw_pitch(0.0f, -0.06f);
  camera_.speed = 3.0f;
  REC_INFO("fur demo: shell-based hair/fur on a sphere");
}

void DemoScenes::CreateAutoLodDemoScene() {
  // One high-poly sphere (~19k tris, a single authored lod) whose coarser lods
  // are generated by the mesh simplifier, then instanced down a receding line so
  // the gpu picks the decimated lods with distance.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/autolod/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -8.1f, 0}});  // top at y = -0.1
  world_.Add(floor, world::Renderable{ground.id});

  asset::Material mat;
  mat.id = asset::MakeAssetId("builtin/autolod/mat");
  mat.base_color_factor[0] = 0.3f;
  mat.base_color_factor[1] = 0.55f;
  mat.base_color_factor[2] = 0.85f;
  mat.roughness_factor = 0.4f;
  if (!config_.headless) renderer_.UploadMaterial(mat);

  asset::Mesh sphere = asset::MakeSphere(1.0f, 80, 120, asset::MakeAssetId("builtin/autolod/mesh"));
  sphere.lods[0].submeshes[0].material = mat.id;
  asset::GenerateLods(&sphere);  // appends the decimated lods
  for (size_t i = 0; i < sphere.lods.size(); ++i) {
    REC_INFO("auto-lod: lod{} = {} tris", i, sphere.lods[i].indices.size() / 3);
  }
  if (!config_.headless) renderer_.UploadMesh(sphere);

  // One mesh, instanced at increasing distance: each instance picks its lod.
  const Vec3 pos[3] = {{-1.7f, 0.9f, 5.0f}, {1.5f, 0.9f, 3.0f}, {-1.0f, 0.9f, 1.0f}};
  for (const Vec3& p : pos) {
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {p.x, p.y, p.z}});
    world_.Add(e, world::Renderable{sphere.id});
  }

  camera_.set_position({0.0f, 1.5f, 6.5f});
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 4.0f;
  REC_INFO("auto-lod demo: decimated lods on a single high-poly sphere");
}

void DemoScenes::CreateOitDemoScene() {
  // Five interpenetrating transparent spheres of different colours; weighted
  // blended oit composites them with no sorting, so every layer shows through.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/oit/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -8.0f, 0}});  // top at y = 0
  world_.Add(floor, world::Renderable{ground.id});

  struct S {
    Vec3 pos;
    Vec3 color;
    f32 alpha;
  };
  const S spheres[5] = {{{-0.45f, 1.2f, 0.25f}, {1.0f, 0.12f, 0.12f}, 0.55f},
                        {{0.45f, 1.2f, -0.2f}, {0.12f, 1.0f, 0.18f}, 0.55f},
                        {{0.0f, 1.2f, 0.5f}, {0.15f, 0.3f, 1.0f}, 0.55f},
                        {{0.0f, 1.65f, 0.0f}, {1.0f, 0.95f, 0.15f}, 0.5f},
                        {{0.0f, 0.75f, 0.1f}, {0.15f, 1.0f, 1.0f}, 0.5f}};
  const f32 radius = 0.6f;
  bool reverse = bool(OitReverse);  // verify order independence
  for (int j = 0; j < 5; ++j) {
    const S& s = spheres[reverse ? 4 - j : j];
    render::WboitInstance inst;
    Mat4 m{};
    m.m[0] = m.m[5] = m.m[10] = radius;
    m.m[15] = 1.0f;
    m.m[12] = s.pos.x;
    m.m[13] = s.pos.y;
    m.m[14] = s.pos.z;
    inst.model = m;
    inst.color[0] = s.color.x;
    inst.color[1] = s.color.y;
    inst.color[2] = s.color.z;
    inst.color[3] = s.alpha;
    oit_instances_.push_back(inst);
  }

  camera_.set_position({0.0f, 1.3f, 4.2f});
  camera_.set_yaw_pitch(0.0f, -0.06f);
  camera_.speed = 3.0f;
  REC_INFO("oit demo: {} overlapping transparent spheres{}", oit_instances_.size(),
           reverse ? " (reversed order)" : "");
}

void DemoScenes::CreateOcclusionDemoScene() {
  // A large wall directly in front of the camera hides a dense grid of small
  // cubes behind it. The gpu hi-z pass culls the hidden cubes against last
  // frame's depth, so the visible-draw count drops to roughly the wall + floor
  // even though every cube is still submitted. Strafe sideways and the cubes
  // reappear as they leave the wall's shadow. Verified via the debug overlay
  // ("opaque draws: N / M visible") and REC_NO_OCCLUSION for the A/B baseline.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = 0.6f;
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId floor_mat = mat("builtin/occl/floor", 0.5f, 0.5f, 0.55f);
  asset::AssetId wall_mat = mat("builtin/occl/wall", 0.7f, 0.3f, 0.2f);
  asset::AssetId cube_mat = mat("builtin/occl/cube", 0.2f, 0.6f, 0.8f);

  auto add_box = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, world::Renderable{mesh.id});
  };

  add_box(asset::MakeBox(8.0f, 0.1f, 8.0f, asset::MakeAssetId("builtin/occl/ground")), floor_mat,
          {0, -0.1f, 0});
  // The occluder: tall and wide enough to fully cover the cube grid's silhouette.
  add_box(asset::MakeBox(3.0f, 2.2f, 0.1f, asset::MakeAssetId("builtin/occl/wall")), wall_mat,
          {0, 1.6f, 2.0f});

  // Grid of small cubes (half-extent 0.04, so the screen footprint stays inside
  // the coarse hi-z's small-object window) clustered behind the wall.
  int idx = 0;
  for (int gx = 0; gx < 12; ++gx) {
    for (int gy = 0; gy < 10; ++gy) {
      f32 x = -1.1f + gx * 0.2f;
      f32 y = 0.7f + gy * 0.2f;
      f32 z = -2.0f - (gx % 3) * 0.6f;
      std::string tag = "builtin/occl/c" + std::to_string(idx++);
      add_box(asset::MakeCube(0.04f, asset::MakeAssetId(tag)), cube_mat, {x, y, z});
    }
  }

  camera_.set_position({0.0f, 1.6f, 6.0f});
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 3.0f;
  REC_INFO("occlusion demo: {} small cubes hidden behind a wall (gpu hi-z cull)", idx);
}

void DemoScenes::CreatePointLightDemoScene() {
  // A grid of white tiles under a row of colored omni lights, with the sun dimmed
  // so the dynamic point lights dominate. Verifies forward point lighting and the
  // light-complexity view (REC_DEBUG_VIEW=15) where light volumes overlap.
  asset::Material floor_mat;
  floor_mat.id = asset::MakeAssetId("builtin/lights/floor");
  floor_mat.base_color_factor[0] = floor_mat.base_color_factor[1] = floor_mat.base_color_factor[2] =
      0.32f;  // mid-dark so colored light reflects as colour, not blown-out white
  floor_mat.roughness_factor = 0.5f;
  floor_mat.metallic_factor = 0.0f;
  if (!config_.headless) renderer_.UploadMaterial(floor_mat);

  asset::Mesh ground = asset::MakeBox(6.0f, 0.1f, 6.0f, asset::MakeAssetId("builtin/lights/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), floor_mat.id});
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -0.1f, 0}});
  world_.Add(floor, world::Renderable{ground.id});

  // A few low bumps so the lights wrap over shapes, not just a flat plane.
  for (int i = 0; i < 5; ++i) {
    f32 x = -3.2f + i * 1.6f;
    std::string tag = "builtin/lights/bump" + std::to_string(i);
    asset::Mesh s = asset::MakeSphere(0.6f, 24, 32, asset::MakeAssetId(tag));
    s.lods[0].submeshes.push_back({0, static_cast<u32>(s.lods[0].indices.size()), floor_mat.id});
    if (!config_.headless) renderer_.UploadMesh(s);
    ecs::Entity e = world_.Create();
    world_.Add(e, world::Transform{.position = {x, 0.3f, 0.0f}});
    world_.Add(e, world::Renderable{s.id});
  }

  const f32 col[8][3] = {{1, 0.2f, 0.2f}, {0.2f, 1, 0.2f}, {0.3f, 0.4f, 1}, {1, 0.9f, 0.2f},
                         {1, 0.3f, 1},    {0.2f, 1, 1},    {1, 0.6f, 0.2f}, {0.6f, 0.4f, 1}};
  for (int i = 0; i < 8; ++i) {
    render::PointLight l;
    l.pos_radius[0] = -3.5f + i * 1.0f;
    l.pos_radius[1] = 0.9f;
    l.pos_radius[2] = -0.5f + (i % 2) * 1.0f;
    l.pos_radius[3] = 2.2f;  // influence radius (overlapping, for the complexity view)
    l.color_intensity[0] = col[i][0];
    l.color_intensity[1] = col[i][1];
    l.color_intensity[2] = col[i][2];
    l.color_intensity[3] = 4.0f;
    demo_lights_.push_back(l);
  }

  // New light types: a spot cone sweeping the left floor, a warm sphere-area
  // lamp, and a cool rect panel washing the right wall of tiles.
  {
    render::PointLight spot;
    spot.pos_radius[0] = -4.5f; spot.pos_radius[1] = 3.0f; spot.pos_radius[2] = 2.5f;
    spot.pos_radius[3] = 9.0f;
    spot.color_intensity[0] = 1.0f; spot.color_intensity[1] = 0.95f;
    spot.color_intensity[2] = 0.8f; spot.color_intensity[3] = 20.0f;
    f32 dir[3] = {0.35f, -0.85f, -0.4f};
    f32 len = std::sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    spot.direction_type[0] = dir[0]/len; spot.direction_type[1] = dir[1]/len;
    spot.direction_type[2] = dir[2]/len; spot.direction_type[3] = 1.0f;  // spot
    spot.params[0] = std::cos(0.28f);  // inner ~16 deg
    spot.params[1] = std::cos(0.45f);  // outer ~26 deg
    demo_lights_.push_back(spot);

    render::PointLight ball;
    ball.pos_radius[0] = 4.6f; ball.pos_radius[1] = 0.7f; ball.pos_radius[2] = 2.2f;
    ball.pos_radius[3] = 6.0f;
    ball.color_intensity[0] = 1.0f; ball.color_intensity[1] = 0.6f;
    ball.color_intensity[2] = 0.25f; ball.color_intensity[3] = 8.0f;
    ball.direction_type[3] = 2.0f;  // sphere area
    ball.params[0] = 0.35f;         // source radius
    demo_lights_.push_back(ball);

    render::PointLight panel;
    panel.pos_radius[0] = 0.0f; panel.pos_radius[1] = 1.6f; panel.pos_radius[2] = -3.2f;
    panel.pos_radius[3] = 8.0f;
    panel.color_intensity[0] = 0.4f; panel.color_intensity[1] = 0.7f;
    panel.color_intensity[2] = 1.0f; panel.color_intensity[3] = 6.0f;
    panel.direction_type[0] = 0.0f; panel.direction_type[1] = 0.0f;
    panel.direction_type[2] = 1.0f; panel.direction_type[3] = 3.0f;  // rect area
    panel.params[0] = 1.4f;  // half width
    panel.params[1] = 0.8f;  // half height
    demo_lights_.push_back(panel);
  }

  renderer_.settings().sun_intensity = 0.25f;  // dim the sun + ibl so the point lights dominate
  renderer_.settings().ibl = false;
  renderer_.settings().ambient = 0.02f;
  camera_.set_position({0.0f, 2.4f, 5.5f});
  camera_.set_yaw_pitch(0.0f, -0.32f);
  camera_.speed = 3.0f;
  REC_INFO("point-light demo: {} dynamic omni lights", demo_lights_.size());
}

void DemoScenes::CreateMeshletDemoScene() {
  // A dense sphere rendered through the mesh-shader meshlet path: the gpu splits
  // it into clusters, frustum/cone-culls them, and tints each a distinct color
  // so the decomposition is visible. The mesh is not a normal Renderable; the
  // renderer draws it via the meshlet pass (watch "meshlet: N meshlets ...").
  asset::Mesh sphere = asset::MakeSphere(1.5f, 64, 128, asset::MakeAssetId("builtin/meshlet/sphere"));
  if (!config_.headless) renderer_.UploadMeshletMesh(sphere);

  camera_.set_position({0.0f, 0.0f, 4.5f});
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 3.0f;
  REC_INFO("meshlet demo: mesh-shader cluster rendering ({} tris)",
           sphere.lods[0].indices.size() / 3);
}

void DemoScenes::CreateMaterialXDemoScene() {
  // One sphere per MaterialX file listed (comma separated) in REC_MTLX, so the
  // imported standard_surface lobes can be eyeballed against the source.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/mtlx/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -8.6f, 0}});  // top at y = -0.6
  world_.Add(floor, world::Renderable{ground.id});

  base::Vector<std::string> paths;
  if (const char* env = Mtlx.get()) {
    std::string s = env, cur;
    for (char c : s) {
      if (c == ',') {
        if (!cur.empty()) paths.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) paths.push_back(cur);
  }
  if (paths.empty()) REC_WARN("mtlx demo: set REC_MTLX=a.mtlx,b.mtlx to load materials");

  int n = static_cast<int>(paths.size());
  for (int i = 0; i < n; ++i) {
    asset::Material mat;
    mat.id = asset::MakeAssetId("builtin/mtlx/mat" + std::to_string(i));
    if (!asset::LoadMaterialX(paths[i], &mat)) continue;
    if (!config_.headless) renderer_.UploadMaterial(mat);
    std::string tag = "builtin/mtlx/sphere" + std::to_string(i);
    asset::Mesh sphere = asset::MakeSphere(0.6f, 40, 60, asset::MakeAssetId(tag));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) renderer_.UploadMesh(sphere);
    ecs::Entity e = world_.Create();
    f32 x = (static_cast<f32>(i) - (n - 1) * 0.5f) * 1.5f;
    world_.Add(e, world::Transform{.position = {x, 0.0f, 0.0f}});
    world_.Add(e, world::Renderable{sphere.id});
  }

  camera_.set_position({0.0f, 0.9f, 4.5f});
  camera_.set_yaw_pitch(0.0f, -0.12f);
  camera_.speed = 3.0f;
  REC_INFO("materialx demo: {} materials", n);
}

void DemoScenes::CreateDemoScene() {
  if (config_.demo_scene == "water") {
    CreateWaterDemoScene();
    return;
  }
  if (config_.demo_scene == "cornell") {
    CreateCornellDemoScene();
    return;
  }
  if (config_.demo_scene == "fur") {
    CreateFurDemoScene();
    return;
  }
  if (config_.demo_scene == "gpuparticles") {
    CreateGpuParticleDemoScene();
    return;
  }
  if (config_.demo_scene == "fire") {
    CreateFireDemoScene();
    return;
  }
  if (config_.demo_scene == "bricks") {
    CreateBrickDemoScene();
    return;
  }
  if (config_.demo_scene == "autolod") {
    CreateAutoLodDemoScene();
    return;
  }
  if (config_.demo_scene == "mtlx") {
    CreateMaterialXDemoScene();
    return;
  }
  if (config_.demo_scene == "oit") {
    CreateOitDemoScene();
    return;
  }
  if (config_.demo_scene == "materials") {
    CreateMaterialDemoScene();
    return;
  }
  if (config_.demo_scene == "gaussian") {
    CreateGaussianDemoScene();
    return;
  }
  if (config_.demo_scene == "lod") {
    CreateLodDemoScene();
    return;
  }
  if (config_.demo_scene == "occlusion") {
    CreateOcclusionDemoScene();
    return;
  }
  if (config_.demo_scene == "meshlet") {
    CreateMeshletDemoScene();
    return;
  }
  if (config_.demo_scene == "lights") {
    CreatePointLightDemoScene();
    return;
  }
  asset::Mesh cube = asset::MakeCube(0.7f, asset::MakeAssetId("builtin/cube"));
  asset::Mesh ground = asset::MakeCube(2.5f, asset::MakeAssetId("builtin/ground"));
  if (!config_.headless) {
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }

#if RECREATION_HAS_NET
  if (!config_.connect_address.empty()) {
    // Clients get their world from server snapshots; the meshes above are
    // uploaded so replicated Renderables resolve. The demo input swings the
    // player cube in a circle to exercise the client-to-server path.
    scheduler_.AddSystem(ecs::Stage::kSim, "demo_input", [this](ecs::World&, f32 dt) {
      if (!ctx_.client_session || !ctx_.client_session->joined()) return;
      demo_input_time_ += dt;
      net::PlayerInput input;
      input.move_x = std::cos(demo_input_time_ * 0.8f) * 0.5f;
      input.move_z = std::sin(demo_input_time_ * 0.8f) * 0.5f;
      input.yaw = demo_input_time_ * 0.8f;
      ctx_.client_session->SetInput(input);
    });
    REC_INFO("no game data given, joining as demo client");
    return;
  }
#endif

  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {-2.4f, 0.5f, 0}});
  world_.Add(entity, world::Renderable{cube.id});
  world_.Add(entity, Spin{});

  // Ground under the cube so raytraced shadows have something to land on.
  ecs::Entity floor = world_.Create();
  world_.Add(floor, world::Transform{.position = {0, -3.6f, 0}});
  world_.Add(floor, world::Renderable{ground.id});

#if RECREATION_HAS_NET
  if (config_.host_server) {
    world_.Add(entity, net::AllocateNetworkId());
    world_.Add(floor, net::AllocateNetworkId());
  }
#endif

  scheduler_.AddSystem(ecs::Stage::kSim, "demo_spin", [](ecs::World& world, f32 dt) {
    world.Each<Spin, world::Transform>([dt](ecs::Entity, Spin& spin, world::Transform& t) {
      spin.angle += spin.speed * dt;
      t.rotation[1] = std::sin(spin.angle * 0.5f);
      t.rotation[3] = std::cos(spin.angle * 0.5f);
    });
  });
  actors_->CreateTestCharacter();
  REC_INFO("no game data given, spinning a cube instead");
}




}  // namespace rec
