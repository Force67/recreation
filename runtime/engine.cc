#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <utility>

#include "asset/gltf_loader.h"
#include "asset/materialx.h"
#include "asset/primitives.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/nif.h"
#include "bethesda/record.h"
#include "bethesda/script_attachment.h"
#include "core/log.h"
#include "core/math.h"
#include "quest/quest_def.h"
#include "world/components.h"
#include "world/interaction.h"
#include "script/papyrus/value.h"

namespace rec {
namespace {

// printf into a std::string. Attributed so the compiler still type-checks the
// format at each call site.
__attribute__((format(printf, 1, 2))) std::string Fmt(const char* fmt, ...) {
  char buf[600];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

struct Spin {
  f32 angle = 0;
  f32 speed = 0.9f;
};

Mat4 TransformMatrix(const world::Transform& transform) {
  return MakeTranslation({transform.position[0], transform.position[1], transform.position[2]}) *
         MakeFromQuat(transform.rotation[0], transform.rotation[1], transform.rotation[2],
                      transform.rotation[3]) *
         MakeScale(transform.scale);
}

}  // namespace

bool Engine::Initialize(const EngineConfig& config, std::unique_ptr<Window> window) {
  config_ = config;
  jobs_ = std::make_unique<JobSystem>();

  if (!config_.headless) {
    window_ = window ? std::move(window) : Window::Create({});
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
    ApplyRenderPreset();
  }

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("debug ui unavailable");
    }
    if (!game_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("game ui unavailable");
    }
  }

  if (physics_.Initialize()) {
    // A small wooden cube every scene can throw around (F key).
    asset::Material wood;
    wood.id = asset::MakeAssetId("builtin/physics_cube/material");
    wood.base_color_factor[0] = 0.42f;
    wood.base_color_factor[1] = 0.26f;
    wood.base_color_factor[2] = 0.14f;
    wood.roughness_factor = 0.75f;
    asset::Mesh cube = asset::MakeCube(0.25f, asset::MakeAssetId("builtin/physics_cube"));
    for (asset::MeshLod& lod : cube.lods) {
      for (asset::Submesh& submesh : lod.submeshes) submesh.material = wood.id;
      if (lod.submeshes.empty()) {
        lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), wood.id});
      }
    }
    physics_cube_mesh_ = cube.id;
    if (!config_.headless) {
      renderer_.UploadMaterial(wood);
      renderer_.UploadMesh(cube);
    }
    scheduler_.AddSystem(ecs::Stage::kSim, "physics", [this](ecs::World& world, f32 dt) {
      physics_.Update(dt);
      for (const PhysicsEntity& body : physics_entities_) {
        world::Transform* transform = world.Get<world::Transform>(body.entity);
        if (!transform) continue;
        Vec3 position;
        f32 rotation[4];
        if (physics_.GetBodyTransform(body.body, &position, rotation)) {
          transform->position[0] = position.x;
          transform->position[1] = position.y;
          transform->position[2] = position.z;
          std::memcpy(transform->rotation, rotation, sizeof(rotation));
        }
      }
    });
  }

  if (!config_.gltf_path.empty()) {
    if (!LoadGltfScene()) return false;
  } else if (!config_.data_dir.empty()) {
    if (!LoadGameData()) return false;
  } else {
    CreateDemoScene();
  }

#if RECREATION_HAS_NET
  if (!StartNetworking()) return false;
#endif

  scheduler_.AddSystem(ecs::Stage::kPostSim, "cell_streaming", [this](ecs::World& world, f32) {
    if (!streamer_) return;
    streamer_->Update(world, camera_.position());
  });

  return true;
}

#if RECREATION_HAS_NET
bool Engine::StartNetworking() {
  net::SessionConfig net_config;
  net_config.port = config_.port;
  net_config.player_name = base::NameString(config_.player_name.c_str());
  net_config.max_clients = config_.max_clients;
  // Joining players replicate as cubes until there are real actor assets.
  net_config.player_mesh = asset::MakeAssetId("builtin/cube").hash;

  if (config_.host_server) {
    auto server = std::make_unique<net::ServerSession>(std::move(net_config));
    if (!server->Start()) return false;
    server_session_ = server.get();
    session_ = std::move(server);
    // Replicate the authoritative quest journal. The source is only called when
    // clients are connected, so the guest round-trip costs nothing while idle.
    // Quest state lives on the guest thread, so we marshal the read onto it.
    if (scripts_ && script_bindings_) {
      server_session_->SetQuestSource([this]() -> std::vector<quest::QuestStatus> {
        if (!scripts_) return {};
        auto* binds = script_bindings_.get();
        return scripts_->guest()
            .SubmitFor([binds](script::papyrus::VirtualMachine&) {
              return binds->quest_system().AllStatuses();
            })
            .get();
      });
      // A client activating a reference runs OnActivate authoritatively here; the
      // resulting quest/world changes replicate back through the usual channels.
      server_session_->SetActivateSink([this](u64 handle) { RaiseActivate(handle); });
    }
    // Stream authoritative NPC transforms; the session deltas them so only the
    // NPCs that actually moved this tick go out.
    server_session_->SetActorSource([this]() { return net::CollectActorStates(world_); });
  } else if (!config_.connect_address.empty()) {
    net_config.address = base::String(config_.connect_address.c_str());
    auto client = std::make_unique<net::ClientSession>(std::move(net_config));
    if (!client->Start()) return false;
    client_session_ = client.get();
    session_ = std::move(client);
    // Mirror the server's journal onto our quest system. ApplyStatus mutates
    // quest state, so it has to run on the guest thread like every other write.
    if (scripts_ && script_bindings_) {
      client_session_->SetQuestSink([this](const quest::QuestStatus& status) {
        if (!scripts_) return;
        auto* binds = script_bindings_.get();
        scripts_->guest().Submit([binds, status](script::papyrus::VirtualMachine&) {
          binds->quest_system().ApplyStatus(status);
        });
        if (std::getenv("REC_NET_QUEST_LOG"))
          REC_INFO("net: applied quest 0x{:x} stage {} complete {}", status.handle, status.stage,
                   status.complete ? 1 : 0);
      });
      // Mirror the host's quest-driven world effects (spawns/moves/disables/
      // cleanup). Runs in the net sim stage on the main thread, which owns the
      // ECS, so applying straight to QuestWorld is safe.
      client_session_->SetWorldCommandSink(
          [this](const std::vector<world::WorldCommand>& cmds) { quest_world_.Apply(cmds); });
      // Mirror authoritative NPC movement onto our existing (cell-loaded) NPC
      // entities, interpolated between updates.
      client_session_->SetActorSink([this](const std::vector<net::ActorState>& actors) {
        net::ApplyActorStates(world_, quest_world_, actors, 0.1f);
      });
    }
  } else {
    return true;
  }

  scheduler_.AddSystem(ecs::Stage::kSim, "net", [this](ecs::World& world, f32 dt) {
    session_->Tick(world, dt);
  });
  if (client_session_) {
    // Remote transforms blend between snapshots. With a renderer that runs
    // per frame; headless clients smooth at the fixed step instead.
    const ecs::Stage stage =
        config_.headless ? ecs::Stage::kPostSim : ecs::Stage::kPreRender;
    scheduler_.AddSystem(stage, "net_interpolation", [](ecs::World& world, f32 dt) {
      net::TickInterpolation(world, dt);
    });
  }
  return true;
}
#endif  // RECREATION_HAS_NET

void Engine::ApplyRenderPreset() {
  render::Device* device = renderer_.device();
  if (!device || device->is_stub()) return;  // no gpu, nothing to tune
  const render::DeviceCaps& caps = device->caps();
  render::QualityPreset resolved = render::ResolvePreset(config_.preset, caps);
  render::RenderSettings tuned = render::PresetSettings(resolved, caps);

  // Explicit reconstruction flags (--no-taa / --upscaler) still win over the
  // preset's choice; --no-rt already gates ray tracing at the device level.
  if (config_.renderer.aa_mode == render::AntiAliasingMode::kNone) {
    tuned.aa_mode = render::AntiAliasingMode::kNone;
    tuned.upscaler = render::UpscalerKind::kNone;
  } else if (config_.renderer.upscaler != render::UpscalerKind::kNone) {
    tuned.upscaler = config_.renderer.upscaler;
    tuned.aa_mode = render::AntiAliasingMode::kUpscaler;
  }

  // Initialize() applied the REC_DEBUG_VIEW / REC_PATHTRACE debug env overrides;
  // carry them through so a preset never silently disables headless captures.
  const render::RenderSettings& env = renderer_.settings();
  tuned.debug_view = env.debug_view;
  if (env.debug_view != render::DebugView::kOff) {
    tuned.auto_exposure = false;
    tuned.exposure = 1.0f;
  }
  if (env.path_trace) tuned.path_trace = true;
  if (env.wireframe) tuned.wireframe = true;  // honor REC_WIREFRAME over the preset
  tuned.ssr = env.ssr;                        // honor REC_SSR over the preset
  tuned.ssgi = env.ssgi;                       // honor REC_SSGI over the preset
  tuned.color_grade = env.color_grade;      // presets never set a grade
  tuned.sun_direction = env.sun_direction;  // honor REC_SUN_DIR over the default
  if (std::getenv("REC_NO_OCCLUSION")) tuned.gpu_occlusion = false;  // a/b baseline

  renderer_.settings() = tuned;
  REC_INFO("render preset: {} ({})", render::PresetName(resolved),
           config_.preset == render::QualityPreset::kAuto ? "auto" : "forced");
}

void Engine::CreateWaterDemoScene() {
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
  constexpr f32 kHalf = 120.0f;
  for (u32 i = 0; i < 4; ++i) {
    asset::Vertex v{};
    v.position[0] = (i & 1) ? kHalf : -kHalf;
    v.position[1] = 0;
    v.position[2] = (i & 2) ? kHalf : -kHalf;
    v.normal[1] = 1;
    v.tangent[0] = 1;
    v.tangent[3] = 1;
    v.uv[0] = v.position[0] / 8.0f;
    v.uv[1] = v.position[2] / 8.0f;
    v.color = 0xffffffff;
    lod.vertices.push_back(v);
  }
  for (u32 index : {0u, 1u, 2u, 1u, 3u, 2u}) lod.indices.push_back(index);
  asset::Submesh submesh;
  submesh.index_count = 6;
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
    if (body) physics_entities_.push_back({body, block});
  }

  // An ember fountain in front of the camera to exercise the particle path.
  particles_enabled_ = true;
  particle_emitter_ = {-7.0f, 0.8f, 0.0f};

  camera_.set_position({-14.0f, 3.0f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, -0.25f);
  REC_INFO("water demo scene");
}

void Engine::CreateMaterialDemoScene() {
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

void Engine::UpdateParticles(f32 dt, render::FrameView& view) {
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

void Engine::CreateGaussianDemoScene() {
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
  if (const char* ply = std::getenv("REC_PLY")) {
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

void Engine::CreateLodDemoScene() {
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

void Engine::CreateCornellDemoScene() {
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

void Engine::CreateGpuParticleDemoScene() {
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

void Engine::CreateFurDemoScene() {
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

void Engine::CreateAutoLodDemoScene() {
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

void Engine::CreateOitDemoScene() {
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
  bool reverse = std::getenv("REC_OIT_REVERSE") != nullptr;  // verify order independence
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

void Engine::CreateOcclusionDemoScene() {
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

void Engine::CreatePointLightDemoScene() {
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

  renderer_.settings().sun_intensity = 0.25f;  // dim the sun + ibl so the point lights dominate
  renderer_.settings().ibl = false;
  renderer_.settings().ambient = 0.02f;
  camera_.set_position({0.0f, 2.4f, 5.5f});
  camera_.set_yaw_pitch(0.0f, -0.32f);
  camera_.speed = 3.0f;
  REC_INFO("point-light demo: {} dynamic omni lights", demo_lights_.size());
}

void Engine::CreateMeshletDemoScene() {
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

void Engine::CreateMaterialXDemoScene() {
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
  if (const char* env = std::getenv("REC_MTLX")) {
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

void Engine::CreateDemoScene() {
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
      if (!client_session_ || !client_session_->joined()) return;
      demo_input_time_ += dt;
      net::PlayerInput input;
      input.move_x = std::cos(demo_input_time_ * 0.8f) * 0.5f;
      input.move_z = std::sin(demo_input_time_ * 0.8f) * 0.5f;
      input.yaw = demo_input_time_ * 0.8f;
      client_session_->SetInput(input);
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
  CreateTestCharacter();
  REC_INFO("no game data given, spinning a cube instead");
}

void Engine::CreateTestCharacter() {
  if (config_.headless) return;
  asset::Skeleton skeleton;
  asset::Mesh mesh;
  asset::MakeSkinnedBiped(asset::MakeAssetId("builtin/biped"), &skeleton, &mesh);
  renderer_.UploadMesh(mesh);

  // Ground the character stands on. Flat slab (top at y = 0.06, the sole rest
  // height) plus a raised step under the left foot so foot IK has something to
  // adapt to. Both get a static collider the IK raycast hits.
  asset::Mesh slab = asset::MakeCube(10.0f, asset::MakeAssetId("builtin/ik_ground"));
  asset::Mesh step = asset::MakeCube(0.4f, asset::MakeAssetId("builtin/ik_step"));
  renderer_.UploadMesh(slab);
  renderer_.UploadMesh(step);
  ecs::Entity ground_e = world_.Create();
  world_.Add(ground_e, world::Transform{.position = {0, -9.94f, 0}});
  world_.Add(ground_e, world::Renderable{slab.id});
  physics_.AddStaticBox({0, -9.94f, 0}, {10.0f, 10.0f, 10.0f});
  ecs::Entity step_e = world_.Create();
  world_.Add(step_e, world::Transform{.position = {0.45f, -0.15f, 0.1f}});
  world_.Add(step_e, world::Renderable{step.id});
  physics_.AddStaticBox({0.45f, -0.15f, 0.1f}, {0.4f, 0.4f, 0.4f});

  // Front view of the figure so the leg adaptation reads clearly (bringup).
  camera_.set_position({0.0f, 1.0f, 3.2f});
  camera_.set_yaw_pitch(0.0f, -0.093f);

  // Centre stage where the fly camera looks by default; the cube spins aside.
  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {0.0f, 0.0f, 0.0f}});

  Actor actor;
  actor.entity = entity;
  actor.skeleton = std::move(skeleton);
  actor.pose.ResetToBind(actor.skeleton);
  actor.speed = 0.0f;  // idle so foot IK on the uneven ground is the focus
  actor.foot_ik = true;
  actor.ik_up = {0, 1, 0};
  actor.ik_forward = {0, 0, 1};
  actor.ankle_height = 0.02f;
  // Character capsule so walk mode (T) can drive it. Capsule centre sits ~0.9m
  // above the entity origin (feet), total height ~1.7m.
  actor.capsule_offset = 0.9f;
  actor.character = physics_.CreateCharacter({0.0f, actor.capsule_offset, 0.0f}, 0.3f, 0.55f);
  ActorPart part;
  part.mesh = mesh.id;
  part.skin = mesh.skin;
  part.remap = anim::BuildBoneRemap(actor.skeleton, part.skin);
  actor.parts.push_back(std::move(part));
  anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
  size_t bone_count = actor.skeleton.bones.size();
  player_actor_ = static_cast<i32>(actors_.size());
  actors_.push_back(std::move(actor));
  REC_INFO("spawned test biped ({} bones); press T to walk it", bone_count);

  if (std::getenv("REC_AUTOWALK")) {
    auto_walk_ = true;
    walk_mode_ = true;
  }
}

bool Engine::LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone) {
  auto bytes = vfs_.Read(asset::NormalizePath(path));
  if (!bytes) {
    REC_WARN("actor part not found: {}", path);
    return false;
  }
  ByteSpan span(bytes->data(), bytes->size());
  asset::AssetId id = asset::MakeAssetId(path);
  bethesda::NifConversion conv = bethesda::ConvertNifSkinnedMesh(span, id, path);
  bool rigid = false;
  if (!conv.mesh || !conv.mesh->skinned || conv.mesh->skin.bones.empty()) {
    // Head/hair are static meshes rigged to a single bone, not skinned.
    if (attach_bone < 0) {
      REC_WARN("actor part {} has no skinned geometry", path);
      return false;
    }
    conv = bethesda::ConvertNifRigid(span, id, path);
    if (!conv.mesh || conv.mesh->lods.empty() || conv.mesh->lods[0].vertices.empty()) {
      REC_WARN("actor part {} has no geometry", path);
      return false;
    }
    rigid = true;
  }
  // Textures, then materials, then mesh: the cell streamer's upload order.
  for (const std::string& tex : conv.texture_paths) {
    if (const asset::Texture* t = assets_->LoadTexture(tex)) renderer_.UploadTexture(*t);
  }
  for (const asset::Material& material : conv.materials) {
    assets_->AddMaterial(material);
    renderer_.UploadMaterial(material);
  }
  renderer_.UploadMesh(*conv.mesh);

  ActorPart part;
  part.mesh = conv.mesh->id;
  if (rigid) {
    part.attach_bone = attach_bone;
    if (attach_bone < static_cast<i32>(actor.bone_model.size())) {
      part.attach_inverse_bind = Inverse(actor.bone_model[attach_bone]);
    }
    REC_INFO("actor part {}: rigid, riding bone {}", path, attach_bone);
  } else {
    part.skin = conv.mesh->skin;
    part.remap = anim::BuildBoneRemap(actor.skeleton, part.skin);
    u32 matched = 0;
    for (i32 b : part.remap) {
      if (b >= 0) ++matched;
    }
    REC_INFO("actor part {}: {} skin bones, {} matched to skeleton", path, part.skin.bones.size(),
             matched);
  }
  actor.parts.push_back(std::move(part));
  return true;
}

base::Vector<std::string> Engine::FindHeadPartModels(u32 part_type, u32 max) {
  base::Vector<std::string> out;
  records_.EachOfType(FourCc('H', 'D', 'P', 'T'),
                      [&](bethesda::GlobalFormId id,
                          const bethesda::RecordStore::StoredRecord&) {
                        if (out.size() >= max) return;
                        bethesda::Record rec;
                        if (!records_.Parse(id, &rec)) return;
                        const bethesda::Subrecord* pnam = rec.Find(FourCc('P', 'N', 'A', 'M'));
                        if (!pnam || pnam->data.size() < 4) return;
                        u32 type = 0;
                        std::memcpy(&type, pnam->data.data(), 4);
                        if (type != part_type) return;
                        std::string edid = rec.GetString(FourCc('E', 'D', 'I', 'D'));
                        if (edid.find("Female") != std::string::npos ||
                            edid.find("Child") != std::string::npos) {
                          return;  // keep it a male adult head
                        }
                        std::string model = rec.GetString(FourCc('M', 'O', 'D', 'L'));
                        if (model.empty()) return;
                        std::string path = asset::NormalizePath(model);
                        if (!path.starts_with("meshes/")) path = "meshes/" + path;
                        out.push_back(path);
                      });
  return out;
}

bool Engine::LoadActorTemplate(Actor* out) {
  const std::string skel_path = "meshes/actors/character/character assets/skeleton.nif";
  auto skel_bytes = vfs_.Read(asset::NormalizePath(skel_path));
  if (!skel_bytes) {
    REC_ERROR("skeleton.nif not found in the mounted archives");
    return false;
  }
  asset::Skeleton skeleton;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                    asset::MakeAssetId(skel_path), &skeleton)) {
    REC_ERROR("failed to parse skeleton.nif");
    return false;
  }
  out->skeleton = std::move(skeleton);
  out->pose.ResetToBind(out->skeleton);
  // Bethesda game space (Z-up, ~70 units/m) -> engine space (Y-up, metres).
  constexpr f32 s = 0.01428f;
  Mat4 basis{};
  basis.m[0] = s;
  basis.m[6] = -s;
  basis.m[9] = s;
  basis.m[15] = 1.0f;
  out->skeleton_to_local = basis;
  out->ik_up = {0, 0, 1};       // Bethesda up
  out->ik_forward = {0, 1, 0};  // Bethesda forward
  // Bind pose up front so rigid parts (head, hair) capture their bone's bind.
  anim::ComputeModelMatrices(out->skeleton, out->pose, &out->bone_model);

  const std::string skinned_parts[] = {
      "meshes/actors/character/character assets/malebody_1.nif",
      "meshes/actors/character/character assets/malehands_1.nif",
      "meshes/actors/character/character assets/malefeet_1.nif",
  };
  bool any = false;
  for (const std::string& p : skinned_parts) any = LoadActorPart(p, *out) || any;
  if (!any) {
    REC_ERROR("no skyrim body parts loaded");
    return false;
  }
  // Head + hair ride the head bone (static meshes, no FaceGen morphs).
  i32 head_bone = out->skeleton.Find("NPC Head [Head]");
  LoadActorPart("meshes/actors/character/character assets/malehead.nif", *out, head_bone);
  for (const std::string& hair : FindHeadPartModels(/*hair=*/3, 24)) {
    if (LoadActorPart(hair, *out, head_bone)) {
      REC_INFO("hair: {}", hair);
      break;
    }
  }
  return true;
}

bool Engine::CreateSkyrimActor() {
  Actor actor;
  if (!LoadActorTemplate(&actor)) return false;
  REC_INFO("loaded skyrim skeleton: {} bones", actor.skeleton.bones.size());

  actor.entity = world_.Create();
  world_.Add(actor.entity, world::Transform{.position = {0, 0, 0}});
  actor.animate = true;
  actor.speed = 0.0f;  // idle while tuning foot ik
  actor.foot_ik = true;
  actor.ankle_height = 6.0f;  // game units: the ankle sits ~6u above the sole

  // Character capsule so walk mode (T) drives the Skyrim actor too. Capsule
  // half-height+radius = 0.85, so the entity origin (feet) sits on the ground.
  actor.capsule_offset = 0.85f;
  actor.character = physics_.CreateCharacter({0.0f, 0.85f, 0.0f}, 0.3f, 0.55f);

  // A ground slab to stand on (top at y = 0, where the skeleton's feet sit),
  // plus a step under one foot so the foot IK has something to adapt to.
  asset::Mesh slab = asset::MakeCube(10.0f, asset::MakeAssetId("builtin/actor_ground"));
  asset::Mesh step = asset::MakeCube(0.45f, asset::MakeAssetId("builtin/actor_step"));
  renderer_.UploadMesh(slab);
  renderer_.UploadMesh(step);
  ecs::Entity ground = world_.Create();
  world_.Add(ground, world::Transform{.position = {0, -10.0f, 0}});
  world_.Add(ground, world::Renderable{slab.id});
  ecs::Entity step_e = world_.Create();
  world_.Add(step_e, world::Transform{.position = {0.5f, -0.33f, 0}});
  world_.Add(step_e, world::Renderable{step.id});
  if (physics_.initialized()) {
    physics_.AddStaticBox({0, -10.0f, 0}, {10, 10, 10});
    physics_.AddStaticBox({0.5f, -0.33f, 0}, {0.45f, 0.45f, 0.45f});
  }

  size_t part_count = actor.parts.size();
  player_actor_ = static_cast<i32>(actors_.size());
  actors_.push_back(std::move(actor));

  // The skeleton faces -Z in engine space, so frame it from the front.
  camera_.set_position({0.0f, 0.95f, -3.0f});
  camera_.set_yaw_pitch(3.14159f, -0.08f);
  REC_INFO("skyrim actor ready ({} body parts); press T to walk it", part_count);
  if (std::getenv("REC_AUTOWALK")) {
    auto_walk_ = true;
    walk_mode_ = true;
  }
  return true;
}

void Engine::UpdateActors(f32 dt) {
  for (Actor& actor : actors_) UpdateOneActor(actor, dt);
  for (auto entry : npc_actors_) UpdateOneActor(entry.value, dt);
}

void Engine::UpdateOneActor(Actor& actor, f32 dt) {
  if (!actor.animate) {
    actor.pose.ResetToBind(actor.skeleton);
    anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
    return;
  }
  actor.locomotion.phase = anim::AdvancePhase(actor.locomotion.phase, actor.speed, dt);
  actor.locomotion.Apply(actor.skeleton, actor.speed, &actor.pose);

  if (actor.foot_ik && physics_.initialized()) {
    const world::Transform* t = world_.Get<world::Transform>(actor.entity);
    Mat4 model = (t ? TransformMatrix(*t) : Mat4::Identity()) * actor.skeleton_to_local;
    Mat4 inv_model = Inverse(model);
    // Foot IK works in model space; bridge to the engine-space physics world.
    auto ground = [&](const Vec3& origin, Vec3* hit, Vec3* normal) -> bool {
      physics::PhysicsWorld::RayHit rh;
      if (!physics_.Raycast(TransformPoint(model, origin), {0, -1, 0}, 3.0f, &rh)) return false;
      *hit = TransformPoint(inv_model, rh.position);
      *normal = Normalize(TransformDir(inv_model, rh.normal));
      return true;
    };
    // Stance weights: at speed the planted foot gets IK while the swing foot is
    // left to lift; when nearly idle both feet plant.
    f32 theta = actor.locomotion.phase * 6.2831853f;
    f32 leg = std::sin(theta);
    f32 idle = std::clamp(1.0f - actor.speed, 0.0f, 1.0f);
    f32 foot_weight[2] = {std::max(idle, std::clamp(0.5f + leg * 2.0f, 0.0f, 1.0f)),
                          std::max(idle, std::clamp(0.5f - leg * 2.0f, 0.0f, 1.0f))};
    anim::SolveFootIk(actor.skeleton, ground, actor.ik_up, actor.ik_forward, actor.ankle_height,
                      foot_weight, &actor.pose, &actor.bone_model);
  } else {
    anim::ComputeModelMatrices(actor.skeleton, actor.pose, &actor.bone_model);
  }
}

void Engine::EmitActorDraws(render::FrameView& view) {
  for (Actor& actor : actors_) EmitOneActor(actor, view);
  for (auto entry : npc_actors_) EmitOneActor(entry.value, view);
}

void Engine::EmitOneActor(Actor& actor, render::FrameView& view) {
  const world::Transform* t = world_.Get<world::Transform>(actor.entity);
  Mat4 model = (t ? TransformMatrix(*t) : Mat4::Identity()) * actor.skeleton_to_local;
  for (ActorPart& part : actor.parts) {
    render::DrawItem item;
    item.mesh = part.mesh.hash;
    if (part.attach_bone >= 0 && part.attach_bone < static_cast<i32>(actor.bone_model.size())) {
      // Rigid part: ride the bone's animated delta from its bind transform.
      item.transform = model * actor.bone_model[part.attach_bone] * part.attach_inverse_bind;
      item.prev_transform = item.transform;
      item.skin_offset = -1;
    } else {
      item.transform = model;
      item.prev_transform = actor.prev_model;
      item.skin_offset = static_cast<i32>(view.bone_matrices.size());
      base::Vector<Mat4> palette;
      anim::BuildSkinPalette(actor.bone_model, part.skin, part.remap, &palette);
      for (const Mat4& m : palette) view.bone_matrices.push_back(m);
    }
    view.draws.push_back(item);
  }
  actor.prev_model = model;
}

void Engine::SyncNpcActors() {
  if (config_.headless) return;  // dedicated server doesn't render NPCs
  // Build the shared rig once; every NPC actor is instanced from it.
  if (!npc_template_) {
    if (npc_template_failed_) return;
    Actor tmpl;
    if (!LoadActorTemplate(&tmpl)) {
      npc_template_failed_ = true;
      REC_WARN("npc rendering disabled: actor template failed to load");
      return;
    }
    tmpl.animate = true;
    tmpl.speed = 0.0f;     // idle
    tmpl.foot_ik = false;  // skip per-NPC ground raycasts
    npc_template_ = std::move(tmpl);
    REC_INFO("npc actor template ready ({} parts)", npc_template_->parts.size());
  }
  // Give every NPC entity without one a skinned actor instance (own pose, GPU
  // meshes shared by hash with the template).
  world_.Each<world::Npc, world::Transform>([&](ecs::Entity e, world::Npc&, world::Transform&) {
    const u64 key = static_cast<u64>(e.generation) << 32 | e.index;
    if (npc_actors_.find(key)) return;
    Actor a = *npc_template_;
    a.entity = e;
    a.character = 0;
    a.pose.ResetToBind(a.skeleton);
    npc_actors_.insert(key, std::move(a));
  });
  // Drop actors whose NPC entity streamed out, so none render at the origin.
  scratch_dead_actors_.clear();
  for (auto entry : npc_actors_)
    if (!world_.IsAlive(entry.value.entity)) scratch_dead_actors_.push_back(entry.key);
  for (u64 key : scratch_dead_actors_) npc_actors_.erase(key);
}

void Engine::SyncSolidBodies() {
  if (config_.headless || !physics_.initialized()) return;
  constexpr f32 kRadius = 0.3f, kHalfHeight = 0.55f;
  constexpr f32 kCentreOffset = kRadius + kHalfHeight;  // feet -> capsule centre
  const ecs::Entity local =
      player_actor_ >= 0 ? actors_[player_actor_].entity : ecs::kInvalidEntity;

  auto ensure = [&](ecs::Entity e, const world::Transform& t) {
    const u64 key = static_cast<u64>(e.generation) << 32 | e.index;
    const Vec3 centre{t.position[0], t.position[1] + kCentreOffset, t.position[2]};
    if (physics::BodyId* body = solid_bodies_.find(key)) {
      physics_.SetBodyPosition(*body, centre, t.rotation);
    } else if (physics::BodyId id = physics_.AddKinematicCapsule(centre, kRadius, kHalfHeight)) {
      solid_bodies_.insert(key, id);
    }
  };

  world_.Each<world::Npc, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::Transform& t) { ensure(e, t); });
  // Other (networked) players are solid too; never block the local player itself.
  world_.Each<net::NetworkId, world::Transform>(
      [&](ecs::Entity e, net::NetworkId&, world::Transform& t) {
        if (e.index == local.index && e.generation == local.generation) return;
        ensure(e, t);
      });

  // Drop capsules whose entity is gone (cell unload / player disconnect).
  scratch_dead_actors_.clear();
  for (auto entry : solid_bodies_) {
    const ecs::Entity e{static_cast<u32>(entry.key & 0xffffffffu), static_cast<u32>(entry.key >> 32)};
    if (!world_.IsAlive(e)) scratch_dead_actors_.push_back(entry.key);
  }
  for (u64 key : scratch_dead_actors_) {
    if (physics::BodyId* body = solid_bodies_.find(key)) physics_.RemoveBody(*body);
    solid_bodies_.erase(key);
  }
}

bool Engine::LoadGameData() {
  game_ = config_.game != bethesda::Game::kUnknown
              ? config_.game
              : bethesda::GameProfile::DetectFromDataDir(config_.data_dir);
  if (game_ == bethesda::Game::kUnknown) {
    REC_ERROR("could not detect a supported game in {}", config_.data_dir);
    return false;
  }
  const auto& profile = bethesda::GameProfile::For(game_);
  REC_INFO("detected {}", profile.name);

  MountArchives();
  // Loose files mount last so they win over archives.
  vfs_.Mount(asset::MakeLooseFileProvider(config_.data_dir));

  assets_ = std::make_unique<asset::AssetDatabase>(vfs_);
  bethesda::RegisterConverters(*assets_, profile);

  auto order = bethesda::LoadOrder::FromPluginsTxt(config_.plugins_txt, profile);
  if (!records_.LoadAll(config_.data_dir, order, profile)) return false;
  REC_INFO("{} plugins, {} records", order.plugins().size(), records_.record_count());

  // Localized string tables, base masters first so their ids win the collisions
  // a single id-keyed table cannot avoid (the main quest text lives in the base
  // game master). Plugins without string files (non-localized) are skipped.
  for (const std::string& plugin : order.plugins()) strings_.Load(vfs_, plugin, "english");
  REC_INFO("loaded {} localized strings", strings_.size());

  // Index dialogue topics by quest so an NPC conversation opens without
  // rescanning every DIAL.
  dialogue_.Build(records_);
  REC_INFO("dialogue: {} topics indexed", dialogue_.topic_count());

  // The Papyrus guest: a separate, single-threaded world that runs game scripts
  // off the main thread. Form natives read the RecordStore; actor values and
  // inventory are backed by the bindings' own stores.
  script_bindings_ = std::make_unique<rec::script::skyrim::RecordBackedSkyrimBindings>(&records_);
  script_bindings_->set_strings(&strings_);
  script_bindings_->set_player(rec::script::papyrus::ObjectRef{0x14});  // Skyrim player ref
  // Route quest-driven world mutations (PlaceAtMe/MoveTo/Enable/Delete + cleanup)
  // through the provenance layer; the player teleports through a host hook since
  // it is an actor/capsule, not a registry entity.
  script_bindings_->set_world_sink(&runtime_world_sink_);
  quest_world_.set_on_move_player([this](f32 x, f32 y, f32 z) { TeleportPlayer(x, y, z); });
  // A connecting client is a passive replica: the server runs the scripts and is
  // authoritative for quest and quest-driven world state; the client mirrors it
  // through replicated quest snapshots and world commands. Definitions still load
  // (for journal text), but the client's own scripts may not mutate that state.
  // The host and single-player stay authoritative.
  script_bindings_->set_replica_mode(!config_.connect_address.empty());
  if (script_bindings_->replica_mode())
    REC_INFO("multiplayer client: quests run server-authoritative (replica mode)");
  scripts_ = std::make_unique<rec::script::ScriptSystem>(game_, &vfs_, script_bindings_.get());
  // Hand the bindings the guest VM so quest stage fragments can execute (run on
  // the guest thread, where the bindings live).
  {
    auto* binds = script_bindings_.get();
    scripts_->guest().Submit(
        [binds](rec::script::papyrus::VirtualMachine& vm) { binds->set_vm(&vm); });
  }
  AttachQuestScripts();

  // Actor bringup scene: load a Skyrim character and animate it, no streaming.
  if (config_.demo_scene == "actor") return CreateSkyrimActor();

  streamer_ = std::make_unique<world::CellStreamer>(records_, *assets_);
  // Register streamed NPCs in the quest world so quests can target them and
  // clients can apply replicated actor transforms by form id.
  streamer_->set_quest_world(&quest_world_);
  if (physics_.initialized()) {
    streamer_->set_physics(&physics_);
    physics_.set_water_height([this](const Vec3& position, f32* height, Vec3* flow) {
      return streamer_->WaterHeightAt(position, height, flow);
    });
  }
  world::CellStreamer::Settings settings;
  settings.grass_density = config_.grass_density;
  streamer_->Configure(settings);
  if (!config_.headless) {
    world::CellStreamer::Uploads uploads;
    uploads.mesh = [this](const asset::Mesh& mesh) { return renderer_.UploadMesh(mesh); };
    uploads.texture = [this](const asset::Texture& texture) {
      return renderer_.UploadTexture(texture);
    };
    uploads.material = [this](const asset::Material& material) {
      return renderer_.UploadMaterial(material);
    };
    streamer_->SetUploads(std::move(uploads));
  }

  if (!config_.interior.empty()) return LoadInterior();
  if (!streamer_->SelectWorldspace(profile.exterior_worldspace)) return false;

  // Drop the camera a bit above the terrain at the middle of the start cell.
  constexpr f32 kUnitsToMeters = 0.01428f;
  constexpr f32 kCellSize = 4096.0f;
  f32 beth_x = (static_cast<f32>(config_.start_cell_x) + 0.5f) * kCellSize;
  f32 beth_y = (static_cast<f32>(config_.start_cell_y) + 0.5f) * kCellSize;
  Vec3 start{beth_x * kUnitsToMeters, 0.0f, -beth_y * kUnitsToMeters};
  f32 ground = 0;
  if (streamer_->GroundHeight(start.x, start.z, &ground)) {
    start.y = ground + 10.0f;  // a little above the terrain for a view
  } else {
    REC_WARN("no terrain at start cell {},{}", config_.start_cell_x, config_.start_cell_y);
  }
  camera_.set_position(start);
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 30.0f;
  REC_INFO("camera start: cell {},{} at ({:.1f}, {:.1f}, {:.1f})", config_.start_cell_x,
           config_.start_cell_y, start.x, start.y, start.z);
  return true;
}

bool Engine::LoadInterior() {
  bethesda::GlobalFormId cell_id;
  if (config_.interior.starts_with("0x") || config_.interior.starts_with("0X")) {
    // Load order form id: top byte is the plugin index for full plugins.
    u32 raw = static_cast<u32>(std::stoul(config_.interior.substr(2), nullptr, 16));
    cell_id = {static_cast<u16>(raw >> 24), raw & 0xffffff};
  } else {
    cell_id = records_.FindInteriorCell(config_.interior);
  }
  if (cell_id.plugin == 0xffff) {
    REC_ERROR("interior cell not found: {}", config_.interior);
    return false;
  }

  Vec3 start{};
  if (!streamer_->LoadInterior(world_, cell_id, &start)) return false;
  camera_.set_position(start);
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 5.0f;
  REC_INFO("camera start: interior {} at ({:.1f}, {:.1f}, {:.1f})", config_.interior, start.x,
           start.y, start.z);
  REC_INFO("interior {}: {} npcs loaded", config_.interior, streamer_->spawned_npc_count());
  return true;
}

void Engine::MountArchives() {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(config_.data_dir, ec)) {
    std::string path = entry.path().string();
    // TODO: archive order should follow plugin order plus the ini resource
    // lists, alphabetical is a placeholder.
    if (auto provider = bethesda::OpenArchive(path)) vfs_.Mount(std::move(provider));
  }
}

void Engine::AttachQuestScripts() {
  if (!scripts_) return;
  // Quests are the game's always-on scripts. Every quest with a Papyrus script
  // is instantiated so the quest browser lists the full set (main quests
  // included), not an arbitrary prefix. config.max_quest_scripts > 0 caps it for
  // a faster bring-up; 0 (the default) attaches them all.
  int limit = config_.max_quest_scripts;
  int quests = 0;
  int instances = 0;
  records_.EachOfType(FourCc('Q', 'U', 'S', 'T'),
                      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
                        if (limit > 0 && quests >= limit) return;
                        bethesda::Record record;
                        if (!records_.Parse(id, &record)) return;
                        const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
                        if (!vmad) return;
                        bethesda::ScriptAttachment attachment;
                        std::vector<bethesda::QuestStageFragment> fragments;
                        if (!bethesda::ParseQuestFragments(vmad->data, &attachment, &fragments) ||
                            attachment.scripts.empty())
                          return;
                        u64 handle = static_cast<u64>(id.plugin) << 32 | id.local_id;
                        auto created = scripts_->AttachScripts(handle, attachment);
                        if (!created.empty()) {
                          ++quests;
                          instances += static_cast<int>(created.size());
                          // Parse the quest's stages and objectives (log text,
                          // objective text, compass targets) for the HUD/debugger.
                          quest::QuestDef def =
                              quest::ParseQuestDefinition(handle, record, &strings_);
                          // Key the record list by editor id: it is the stable
                          // handle REC_START_QUEST and the debugger match on. The
                          // panel's display name comes from the quest definition.
                          std::string edid =
                              !def.editor_id.empty() ? def.editor_id : std::to_string(id.local_id);
                          quest_records_.push_back({handle, std::move(edid)});
                          // Register the stage->fragment map and definition on the
                          // guest thread (the bindings' only caller) so SetStage runs
                          // the quest's authored logic and snapshots carry its text.
                          auto* binds = script_bindings_.get();
                          scripts_->guest().Submit(
                              [binds, handle, def = std::move(def),
                               fragments = std::move(fragments)](
                                  rec::script::papyrus::VirtualMachine&) mutable {
                                binds->quest_system().SetDefinition(std::move(def));
                                for (const auto& f : fragments)
                                  binds->SetStageFragment(handle, f.stage, f.function);
                              });
                        }
                      });
  REC_INFO("papyrus: instantiated {} scripts across {} quests, {} script types loaded", instances,
           quests, scripts_->loaded_script_count());

  // REC_START_QUEST=<EDID>[:<stage>] starts a quest at load (runs its opening
  // stage fragment) so quest logic can be exercised without the UI. The optional
  // :stage drives it to that stage, e.g. REC_START_QUEST=MQ101:160 to surface an
  // objective on the HUD.
  if (const char* want = std::getenv("REC_START_QUEST")) {
    std::string spec = want;
    std::string edid = spec;
    i32 start_stage = -1;
    if (size_t colon = spec.find(':'); colon != std::string::npos) {
      edid = spec.substr(0, colon);
      start_stage = std::atoi(spec.c_str() + colon + 1);
    }
    auto* binds = script_bindings_.get();
    int started = 0;
    for (const auto& [handle, name] : quest_records_) {
      if (edid != "all" && name != edid) continue;
      scripts_->guest().Submit([binds, h = handle, start_stage](rec::script::papyrus::VirtualMachine&) {
        rec::script::papyrus::ObjectRef ref{h};
        binds->StartQuest(ref);
        if (start_stage >= 0) binds->SetStage(ref, start_stage);
      });
      // Open the debugger on the started quest so its stages/objectives show.
      if (edid != "all") quest_panel_.selected = handle;
      ++started;
      if (edid != "all") break;
    }
    REC_INFO("debug: started {} quest(s) matching '{}'", started, edid);
  }

  // REC_QUEST_REPORT=<EDID> drives a quest through its stages to its completion
  // stage and prints the journey, then quits. A headless, deterministic check
  // that a quest (e.g. the first main quest, MQ101) runs to completion.
  if (const char* want = std::getenv("REC_QUEST_REPORT")) {
    ReportQuestToCompletion(want);
    quit_.store(true, std::memory_order_relaxed);
  }

  // REC_DIALOGUE_REPORT=<EDID> dumps a quest's dialogue topics/responses, then
  // quits. Confirms DIAL/INFO parsing (and fragment offsets) against real data.
  if (const char* want = std::getenv("REC_DIALOGUE_REPORT")) {
    ReportDialogue(want);
    quit_.store(true, std::memory_order_relaxed);
  }
}

void Engine::ReportDialogue(const std::string& edid) {
  u64 handle = 0;
  for (const auto& [h, name] : quest_records_) {
    if (name == edid) {
      handle = h;
      break;
    }
  }
  if (handle == 0) {
    std::printf("dialogue report: no quest matching '%s'\n", edid.c_str());
    return;
  }
  const std::vector<dialogue::Handle>& topics = dialogue_.TopicsForQuest(handle);
  std::printf("=== dialogue for %s (0x%llx): %zu topics ===\n", edid.c_str(),
              static_cast<unsigned long long>(handle), topics.size());
  int with_fragment = 0;
  int with_conditions = 0;
  for (dialogue::Handle t : topics) {
    bethesda::GlobalFormId dial{static_cast<u16>(t >> 32), static_cast<u32>(t & 0xffffffffu)};
    dialogue::Topic topic = dialogue::ParseTopic(records_, dial, &strings_);
    std::printf("topic [%s] \"%s\" (%zu responses)\n", topic.editor_id.c_str(),
                topic.text.c_str(), topic.responses.size());
    for (const dialogue::Response& r : topic.responses) {
      std::printf("  player: \"%s\"\n  npc:    \"%s\"\n", r.player_line.c_str(),
                  r.npc_line.c_str());
      if (!r.fragment_script.empty()) {
        ++with_fragment;
        std::printf("  fragment: %s.%s\n", r.fragment_script.c_str(),
                    r.fragment_function.c_str());
      }
      if (!r.conditions.empty()) {
        ++with_conditions;
        std::printf("  conditions: %zu\n", r.conditions.comparisons.size());
      }
    }
  }
  std::printf("=== %d responses carry a fragment, %d carry conditions ===\n", with_fragment,
              with_conditions);
  std::fflush(stdout);
}

void Engine::ReportQuestToCompletion(const std::string& edid) {
  u64 handle = 0;
  for (const auto& [h, name] : quest_records_) {
    if (name == edid) {
      handle = h;
      break;
    }
  }
  if (handle == 0) {
    std::printf("quest report: no quest matching '%s'\n", edid.c_str());
    return;
  }

  auto* binds = script_bindings_.get();
  // Drive and snapshot on the guest thread (the bindings' only legal caller);
  // build the human-readable report there and print it on return.
  std::string report =
      scripts_->guest()
          .SubmitFor([binds, handle](rec::script::papyrus::VirtualMachine&) {
            using rec::script::papyrus::ObjectRef;
            quest::QuestSystem& qs = binds->quest_system();
            const ObjectRef ref{handle};
            std::string r;
            auto emit = [&](const std::string& line) {
              r += line;
              r += '\n';
            };

            const quest::QuestDef* def = qs.Definition(handle);
            emit(Fmt("=== quest report: %s (0x%llx) ===", def ? def->editor_id.c_str() : "?",
                     static_cast<unsigned long long>(handle)));
            if (!def) {
              emit("no definition parsed");
              return r;
            }
            emit(Fmt("name: %s", def->name.empty() ? "(none)" : def->name.c_str()));
            emit(Fmt("priority %d, %zu stages, %zu objectives, completion stage %d", def->priority,
                     def->stages.size(), def->objectives.size(), def->CompletionStage()));
            for (const quest::StageDef& s : def->stages)
              emit(Fmt("  stage %d%s %s", s.index, s.complete_quest ? " [completes]" : "",
                       s.log_entry.c_str()));
            for (const quest::ObjectiveDef& o : def->objectives)
              emit(Fmt("  objective %d: %s", o.index, o.text.c_str()));

            emit("driving to completion:");
            binds->StartQuest(ref);
            emit(Fmt("  start -> running=%d stage=%d", qs.IsRunning(handle), qs.GetStage(handle)));
            // Walk the defined stages in ascending order; each SetStage runs the
            // stage's authored fragment (objectives, ref enables, chained stages).
            std::vector<i32> order;
            for (const quest::StageDef& s : def->stages) order.push_back(s.index);
            std::sort(order.begin(), order.end());
            order.erase(std::unique(order.begin(), order.end()), order.end());
            for (i32 stage : order) {
              binds->SetStage(ref, stage);
              std::string shown;
              for (const quest::ObjectiveDef& o : def->objectives)
                if (qs.IsObjectiveDisplayed(handle, o.index))
                  shown += Fmt(" [%d]", o.index);
              emit(Fmt("  set stage %d -> stage=%d complete=%d displayed:%s", stage,
                       qs.GetStage(handle), qs.IsComplete(handle),
                       shown.empty() ? " none" : shown.c_str()));
            }
            const i32 cs = def->CompletionStage();
            if (cs >= 0 && !qs.IsComplete(handle)) {
              binds->SetStage(ref, cs);
              emit(Fmt("  set completion stage %d -> complete=%d", cs, qs.IsComplete(handle)));
            }

            quest::QuestStatus st = qs.Status(handle);
            emit(Fmt("result: running=%d active=%d stage=%d complete=%s", st.running, st.active,
                     st.stage, st.complete ? "YES" : "no"));
            for (const quest::ObjectiveStatus& o : st.objectives)
              emit(Fmt("  objective %d: displayed=%d completed=%d  %s", o.index, o.displayed,
                       o.completed, o.text.c_str()));
            return r;
          })
          .get();
  std::printf("%s", report.c_str());
  std::fflush(stdout);
}

std::string Engine::ActivationLabel(bethesda::GlobalFormId refr) {
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(refr);
  if (!stored) return "Activate";
  bethesda::Record record;
  if (!records_.Parse(refr, &record)) return "Activate";

  // The placed reference points at its base object through NAME; the base
  // carries the displayed name and the type that picks the verb.
  std::string verb = "Activate";
  std::string name;
  if (const bethesda::Subrecord* nm = record.Find(FourCc('N', 'A', 'M', 'E')); nm && nm->data.size() >= 4) {
    u32 raw;
    std::memcpy(&raw, nm->data.data(), 4);
    bethesda::GlobalFormId base = records_.ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
    if (const bethesda::RecordStore::StoredRecord* bstored = records_.Find(base)) {
      if (bstored->header.type == FourCc('N', 'P', 'C', '_') ||
          bstored->header.type == FourCc('A', 'C', 'H', 'R'))
        verb = "Talk to";
      bethesda::Record brecord;
      if (records_.Parse(base, &brecord)) {
        if (const bethesda::Subrecord* full = brecord.Find(FourCc('F', 'U', 'L', 'L'))) {
          if (full->data.size() >= 4) {
            u32 string_id;
            std::memcpy(&string_id, full->data.data(), 4);
            if (const base::String* s = strings_.Find(string_id)) name = std::string(s->c_str());
          }
          if (name.empty()) name = brecord.GetString(FourCc('F', 'U', 'L', 'L'));
        }
      }
    }
  }
  return name.empty() ? verb : verb + " " + name;
}

void Engine::UpdateInteraction(bool activate_pressed) {
  // Activation is a walk-mode affordance and needs the guest to dispatch events.
  if (!walk_mode_ || player_actor_ < 0 || !scripts_ || !script_bindings_) {
    if (activate_target_ != 0) {
      activate_target_ = 0;
      game_ui_.SetActivatePrompt("");
    }
    return;
  }

  // Skyrim's activation reach is ~150 units; the world is meters (~70 units/m).
  constexpr f32 kRange = 2.2f;
  constexpr f32 kFacingDot = 0.45f;  // ~63 degree cone
  // Reach from the player's head, not the camera: in third person the eye sits
  // metres behind the body and would put everything in front out of range.
  Vec3 eye = walk_eye_;
  if (world::Transform* t = world_.Get<world::Transform>(actors_[player_actor_].entity))
    eye = Vec3{t->position[0], t->position[1] + 1.6f, t->position[2]};
  const Vec3 fwd = Normalize(walk_target_ - walk_eye_);

  // Collect the form-linked refs near the eye; a coarse cull keeps this cheap
  // even in a dense cell, and the cap bounds a pathological frame.
  base::Vector<world::ActivationCandidate> candidates;
  const f32 coarse_sq = (kRange + 0.5f) * (kRange + 0.5f);
  world_.Each<world::FormLink, world::Transform>(
      [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
        if (candidates.size() >= 512) return;
        const f32 dx = t.position[0] - eye.x, dy = t.position[1] - eye.y, dz = t.position[2] - eye.z;
        if (dx * dx + dy * dy + dz * dz > coarse_sq) return;
        world::ActivationCandidate c;
        c.form_handle = link.form.packed();
        c.pos[0] = t.position[0];
        c.pos[1] = t.position[1];
        c.pos[2] = t.position[2];
        candidates.push_back(c);
      });

  const float p[3] = {eye.x, eye.y, eye.z};
  const float f[3] = {fwd.x, fwd.y, fwd.z};
  const int idx = world::PickActivationTarget(p, f, candidates.data(),
                                              static_cast<int>(candidates.size()), kRange, kFacingDot);
  if (idx < 0) {
    if (activate_target_ != 0) {
      activate_target_ = 0;
      game_ui_.SetActivatePrompt("");
    }
    return;
  }

  const u64 handle = candidates[idx].form_handle;
  if (handle != activate_target_) {
    activate_target_ = handle;
    bethesda::GlobalFormId form{static_cast<u16>(handle >> 32),
                                static_cast<u32>(handle & 0xffffffffu)};
    activate_label_ = ActivationLabel(form);
    game_ui_.SetActivatePrompt(activate_label_ + "   [E]");
  }

  if (activate_pressed) {
    REC_INFO("activate: {} (0x{:x})", activate_label_, handle);
    // A multiplayer client is not authoritative: it asks the server to run the
    // activation (dialogue/quest logic) and receives the results replicated. The
    // host and single-player raise it directly.
    if (client_session_ && client_session_->joined())
      client_session_->SendActivate(handle);
    else
      RaiseActivate(handle);
  }
}

void Engine::RaiseActivate(u64 handle) {
  if (!scripts_) return;
  // OnActivate(player). Scripted activators and NPCs run their authored response,
  // which can set quest stages. The activator is the single Skyrim player ref;
  // multiplayer treats any client's activation as "the player" for quest logic.
  scripts_->guest().RaiseEvent(
      script::papyrus::ObjectRef{handle}, "OnActivate",
      {script::papyrus::Value::Object(script::papyrus::ObjectRef{0x14})});
}

void Engine::RefreshQuestPanel(f32 dt) {
  if (!scripts_ || !script_bindings_ || quest_records_.empty()) {
    quest_panel_.available = false;
    return;
  }
  quest_panel_.available = true;

  // Mutations run on the guest thread (the bindings' only legal caller).
  if (!quest_panel_.set_running) {
    quest_panel_.set_running = [this](u64 handle, bool run) {
      auto* binds = script_bindings_.get();
      scripts_->guest().Submit([binds, handle, run](script::papyrus::VirtualMachine&) {
        if (run)
          binds->StartQuest(script::papyrus::ObjectRef{handle});
        else
          binds->StopQuest(script::papyrus::ObjectRef{handle});
      });
    };
    quest_panel_.set_stage = [this](u64 handle, i32 stage) {
      auto* binds = script_bindings_.get();
      scripts_->guest().Submit([binds, handle, stage](script::papyrus::VirtualMachine&) {
        binds->SetStage(script::papyrus::ObjectRef{handle}, stage);
      });
    };
    quest_panel_.set_objective_displayed = [this](u64 handle, i32 objective, bool displayed) {
      auto* binds = script_bindings_.get();
      scripts_->guest().Submit([binds, handle, objective, displayed](script::papyrus::VirtualMachine&) {
        binds->SetObjectiveDisplayed(script::papyrus::ObjectRef{handle}, objective, displayed);
      });
    };
    quest_panel_.set_objective_completed = [this](u64 handle, i32 objective, bool completed) {
      auto* binds = script_bindings_.get();
      scripts_->guest().Submit([binds, handle, objective, completed](script::papyrus::VirtualMachine&) {
        binds->SetObjectiveCompleted(script::papyrus::ObjectRef{handle}, objective, completed);
      });
    };
  }

  // Snapshot the live state at a few Hz; one guest round-trip serves both the
  // debug panel (every quest, lightweight) and the HUD (only running quests,
  // with their objective text).
  quest_ui_timer_ -= dt;
  if (!quest_panel_.quests.empty() && quest_ui_timer_ > 0.0f) return;
  quest_ui_timer_ = 0.2f;
  auto* binds = script_bindings_.get();
  base::Vector<std::pair<u64, std::string>> src = quest_records_;
  u64 selected = quest_panel_.selected;

  struct Snapshot {
    std::vector<QuestPanel::Quest> panel;
    std::vector<quest::QuestStatus> running;
    QuestPanel::Detail detail;
  };
  Snapshot snap =
      scripts_->guest()
          .SubmitFor([binds, src, selected](script::papyrus::VirtualMachine&) {
            const quest::QuestSystem& qs = binds->quest_system();
            Snapshot out;
            out.panel.reserve(src.size());
            for (const auto& [handle, edid] : src) {
              const quest::QuestDef* def = qs.Definition(handle);
              std::string name = (def && !def->name.empty()) ? def->name : edid;
              out.panel.push_back({std::move(name), handle, qs.IsRunning(handle),
                                   qs.IsActive(handle), qs.IsComplete(handle), qs.GetStage(handle)});
            }
            out.running = qs.RunningStatuses();
            // Expand the selected quest into stages and objectives for the debugger.
            if (selected != 0) {
              out.detail.handle = selected;
              if (const quest::QuestDef* def = qs.Definition(selected)) {
                out.detail.editor_id = def->editor_id;
                out.detail.completion_stage = def->CompletionStage();
                for (const quest::StageDef& s : def->stages)
                  out.detail.stages.push_back({s.index, s.log_entry, qs.GetStageDone(selected, s.index)});
              }
              quest::QuestStatus st = qs.Status(selected);
              for (const quest::ObjectiveStatus& o : st.objectives)
                out.detail.objectives.push_back({o.index, o.text, o.displayed, o.completed});
            }
            return out;
          })
          .get();
  quest_panel_.quests = std::move(snap.panel);
  quest_panel_.detail = std::move(snap.detail);
  UpdateQuestHud(snap.running);
}

void Engine::UpdateQuestHud(const std::vector<quest::QuestStatus>& running) {
  // Track the most recently changed running quest, the one the player is
  // actively progressing.
  const quest::QuestStatus* tracked = nullptr;
  for (const quest::QuestStatus& q : running) {
    if (!tracked || q.revision > tracked->revision) tracked = &q;
  }

  if (!tracked) {
    if (hud_tracked_quest_ != 0) {
      hud_tracked_quest_ = 0;
      hud_tracked_revision_ = 0;
      game_ui_.SetQuest(HudQuest{});
    }
    return;
  }

  HudQuest hud;
  hud.title = tracked->name;
  for (const quest::ObjectiveStatus& o : tracked->objectives) {
    if (!o.displayed && !o.completed) continue;
    hud.objectives.push_back({o.text, o.completed});
  }
  game_ui_.SetQuest(hud);

  // Raise the banner once per change: when the tracked quest switches or its
  // revision advances.
  if (tracked->handle != hud_tracked_quest_ || tracked->revision != hud_tracked_revision_) {
    if (hud_tracked_revision_ != 0 || tracked->handle != hud_tracked_quest_)
      game_ui_.FlashQuestUpdate(tracked->complete ? tracked->name + " (Complete)" : tracked->name);
    hud_tracked_quest_ = tracked->handle;
    hud_tracked_revision_ = tracked->revision;
  }
}

void Engine::RefreshNativeTrace(f32 dt) {
  if (!scripts_) {
    native_trace_panel_.available = false;
    return;
  }
  native_trace_panel_.available = true;
  if (!native_trace_panel_.clear) {
    native_trace_panel_.clear = [this] {
      scripts_->guest().Submit(
          [](script::papyrus::VirtualMachine& vm) { vm.ClearNativeTrace(); });
    };
  }

  // Tracing copies two strings per native call, so only run it while the window
  // is open; flip the guest's flag when the visibility changes.
  bool want = debug_ui_.trace_visible();
  if (want != native_trace_on_) {
    native_trace_on_ = want;
    scripts_->guest().Submit(
        [want](script::papyrus::VirtualMachine& vm) { vm.set_native_trace(want); });
  }
  if (!want) return;

  trace_ui_timer_ -= dt;
  if (!native_trace_panel_.recent.empty() && trace_ui_timer_ > 0.0f) return;
  trace_ui_timer_ = 0.15f;

  using NativeCall = script::papyrus::VirtualMachine::NativeCall;
  auto snap = scripts_->guest()
                  .SubmitFor([](script::papyrus::VirtualMachine& vm) {
                    return std::pair<u64, std::vector<NativeCall>>(vm.native_call_count(),
                                                                   vm.native_trace_log());
                  })
                  .get();
  native_trace_panel_.total = snap.first;
  const std::vector<NativeCall>& log = snap.second;

  native_trace_panel_.recent.clear();
  native_trace_panel_.recent.reserve(log.size());
  for (auto it = log.rbegin(); it != log.rend(); ++it)
    native_trace_panel_.recent.push_back(it->script_type + "." + it->function);

  std::unordered_map<std::string, u32> counts;
  for (const NativeCall& c : log) ++counts[c.script_type + "." + c.function];
  std::vector<std::pair<std::string, u32>> top(counts.begin(), counts.end());
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  if (top.size() > 40) top.resize(40);
  native_trace_panel_.top = std::move(top);
}

bool Engine::LoadGltfScene() {
  asset::GltfScene scene;
  if (!asset::LoadGltfScene(config_.gltf_path, &scene)) return false;

  if (!config_.headless) {
    for (const asset::Texture& texture : scene.textures) {
      if (texture.id) renderer_.UploadTexture(texture);
    }
    for (const asset::Material& material : scene.materials) renderer_.UploadMaterial(material);
    for (const asset::Mesh& mesh : scene.meshes) renderer_.UploadMesh(mesh);
  }

  for (const asset::GltfScene::Instance& instance : scene.instances) {
    ecs::Entity entity = world_.Create();
    world::Transform transform;
    transform.position[0] = instance.position.x;
    transform.position[1] = instance.position.y;
    transform.position[2] = instance.position.z;
    std::memcpy(transform.rotation, instance.rotation, sizeof(transform.rotation));
    transform.scale = instance.scale;
    world_.Add(entity, transform);
    world_.Add(entity, world::Renderable{scene.meshes[instance.mesh_index].id});
  }

  // Sponza-friendly start: inside the atrium looking down the long axis.
  camera_.set_position({-7.0f, 1.7f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, 0.0f);
  camera_.speed = 4.0f;
  return true;
}

void Engine::UpdateCamera(f32 frame_delta) {
  if (!window_) return;
  const InputState& input = window_->input();

  // The pause menu freezes the camera and frees the cursor so it can click.
  bool menu = game_ui_.menu_open();
  bool kb = debug_ui_.wants_keyboard();

  if (input.key_pressed(Key::kT) && !menu && !kb && player_actor_ >= 0) {
    walk_mode_ = !walk_mode_;
    REC_INFO("walk mode {}", walk_mode_ ? "on (WASD move, Shift run, Space jump, C view)" : "off");
  }
  if (input.key_pressed(Key::kC) && !menu && !kb) third_person_ = !third_person_;

  if (walk_mode_ && player_actor_ >= 0) {
    WalkUpdate(frame_delta, !menu && !kb);
    UpdateInteraction(input.key_pressed(Key::kE) && !menu && !kb);
  } else {
    bool allow_mouse = !menu && (!debug_ui_.wants_mouse() || camera_.looking());
    bool allow_keyboard = !menu && !kb;
    camera_.Update(input, allow_mouse, allow_keyboard, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    UpdateInteraction(false);  // clears any stale prompt outside walk mode
  }
  DriveCamera(frame_delta);  // orbit / replay overrides + record

  if (input.key_pressed(Key::kF1) && !kb) debug_ui_.ToggleVisible();
  if (input.key_pressed(Key::kF2) && !kb) debug_ui_.ToggleTrace();
  if (input.key_pressed(Key::kF3) && !kb) debug_ui_.ToggleQuests();
  if (input.key_pressed(Key::kF) && !menu && !kb && !walk_mode_) ThrowPhysicsCube();
  if (input.key_pressed(Key::kEscape) && !kb) game_ui_.ToggleMenu();
  if (game_ui_.quit_requested()) RequestQuit();
}

void Engine::LookCameraAt(const Vec3& eye, const Vec3& center) {
  camera_.set_position(eye);
  Vec3 d = Normalize(center - eye);
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z),
                        std::asin(std::clamp(d.y, -1.0f, 1.0f)));  // forward() convention
}

void Engine::DriveCamera(f32 dt) {
  if (!cam_init_) {
    cam_init_ = true;
    cam_orbit_ = std::getenv("REC_ORBIT") != nullptr;
    if (const char* r = std::getenv("REC_RECORD")) cam_record_ = std::fopen(r, "wb");
    if (const char* p = std::getenv("REC_REPLAY")) {
      if (std::FILE* f = std::fopen(p, "rb")) {
        f32 rec[7];
        while (std::fread(rec, sizeof(f32), 7, f) == 7) {
          cam_replay_.push_back({rec[0], {rec[1], rec[2], rec[3]}, {rec[4], rec[5], rec[6]}});
        }
        std::fclose(f);
        REC_INFO("camera replay: {} keys from {}", cam_replay_.size(), p);
      }
    }
  }
  cam_time_ += dt;

  if (cam_orbit_) {
    f32 a = cam_time_ * 0.4f;  // radians/sec
    Vec3 center{0.0f, 1.0f, 0.0f};
    LookCameraAt({center.x + std::cos(a) * 6.0f, 2.4f, center.z + std::sin(a) * 6.0f}, center);
  } else if (!cam_replay_.empty()) {
    // Linear interpolation between the bracketing keys for the current time.
    const CamKey* lo = &cam_replay_[0];
    const CamKey* hi = lo;
    for (const CamKey& k : cam_replay_) {
      if (k.t <= cam_time_) lo = &k;
      if (k.t >= cam_time_) {
        hi = &k;
        break;
      }
    }
    f32 span = hi->t - lo->t;
    f32 u = span > 1e-5f ? std::clamp((cam_time_ - lo->t) / span, 0.0f, 1.0f) : 0.0f;
    auto mix = [&](const Vec3& a, const Vec3& b) { return a + (b - a) * u; };
    LookCameraAt(mix(lo->pos, hi->pos), mix(lo->target, hi->target));
  }

  if (cam_record_) {
    Vec3 p = camera_.position(), t = camera_.target();
    f32 rec[7] = {cam_time_, p.x, p.y, p.z, t.x, t.y, t.z};
    std::fwrite(rec, sizeof(f32), 7, cam_record_);
    std::fflush(cam_record_);  // survive a timeout kill
  }
}

void Engine::WalkUpdate(f32 dt, bool allow) {
  Actor& actor = actors_[player_actor_];
  const InputState& input = window_->input();
  window_->SetRelativeMouseMode(true);  // FPS-style mouse look in walk mode

  if (allow) {
    cam_yaw_ += input.mouse_dx * camera_.sensitivity;
    cam_pitch_ = std::clamp(cam_pitch_ - input.mouse_dy * camera_.sensitivity, -1.4f, 1.4f);
  }

  // Move relative to where the camera faces (flattened to the ground plane).
  Vec3 fwd{std::sin(cam_yaw_), 0, -std::cos(cam_yaw_)};
  Vec3 right{std::cos(cam_yaw_), 0, std::sin(cam_yaw_)};
  Vec3 move{};
  if (allow) {
    if (input.key(Key::kW)) move = move + fwd;
    if (input.key(Key::kS)) move = move - fwd;
    if (input.key(Key::kD)) move = move + right;
    if (input.key(Key::kA)) move = move - right;
  }
  f32 speed = (allow && input.key(Key::kLeftShift)) ? 4.8f : 1.8f;
  if (auto_walk_) move = fwd;  // test hook: walk forward automatically
  f32 move_len = Length(move);
  Vec3 velocity{};
  if (move_len > 0.01f) {
    move = move * (1.0f / move_len);
    velocity = move * speed;
    actor.yaw = std::atan2(move.x, move.z);  // the biped's +Z faces movement
  }
  bool jump = allow && input.key_pressed(Key::kSpace);

  Vec3 char_pos{};
  bool grounded = false;
  physics_.MoveCharacter(actor.character, velocity, jump, dt, &char_pos, &grounded);
  actor.speed = move_len > 0.01f ? speed : 0.0f;
  (void)grounded;

  // Drive the entity from the capsule; the biped's body sits below the centre.
  if (world::Transform* t = world_.Get<world::Transform>(actor.entity)) {
    t->position[0] = char_pos.x;
    t->position[1] = char_pos.y - actor.capsule_offset;
    t->position[2] = char_pos.z;
    f32 h = actor.yaw * 0.5f;
    t->rotation[0] = 0;
    t->rotation[1] = std::sin(h);
    t->rotation[2] = 0;
    t->rotation[3] = std::cos(h);
  }

  Vec3 body{char_pos.x, char_pos.y - actor.capsule_offset, char_pos.z};
  Vec3 cam_fwd{std::cos(cam_pitch_) * std::sin(cam_yaw_), std::sin(cam_pitch_),
               -std::cos(cam_pitch_) * std::cos(cam_yaw_)};
  if (third_person_) {
    Vec3 pivot = body + Vec3{0, 1.5f, 0};
    walk_eye_ = pivot - cam_fwd * 3.2f;
    walk_target_ = pivot;
  } else {
    walk_eye_ = body + Vec3{0, 1.7f, 0};
    walk_target_ = walk_eye_ + cam_fwd;
  }
}

void Engine::ThrowPhysicsCube() {
  if (!physics_.initialized() || !physics_cube_mesh_) return;
  Vec3 forward = camera_.forward();
  Vec3 origin = camera_.position() + forward * 0.8f;
  // Wood-ish density: heavy enough to splash, light enough to float.
  physics::BodyId body = physics_.AddDynamicBox(origin, {0.25f, 0.25f, 0.25f}, 350.0f,
                                                forward * 14.0f);
  if (!body) return;
  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {origin.x, origin.y, origin.z}});
  world_.Add(entity, world::Renderable{physics_cube_mesh_});
  physics_entities_.push_back({body, entity});
}

void Engine::TeleportPlayer(f32 x, f32 y, f32 z) {
  if (player_actor_ < 0) return;
  Actor& a = actors_[player_actor_];
  if (a.character) physics_.SetCharacterPosition(a.character, Vec3{x, y, z});
  if (world::Transform* t = world_.Get<world::Transform>(a.entity)) {
    t->position[0] = x;
    t->position[1] = y;
    t->position[2] = z;
  }
  REC_INFO("quest: teleported player to ({:.1f}, {:.1f}, {:.1f})", x, y, z);
}

void Engine::ApplyQuestWorld() {
  std::vector<world::WorldCommand> commands = quest_world_queue_.Drain();
  if (commands.empty()) return;
  quest_world_.Apply(commands);  // host/single-player: apply locally + record provenance
  if (server_session_) server_session_->SendWorldCommands(commands);  // mirror to clients
}

void Engine::ServerSimulateActors(f32 /*dt*/) {
  // Pushers: the local player (listen server / single-player) plus every
  // networked player. A player that is both contributes twice, which is
  // harmless (the second shove is a no-op once the first cleared the radius).
  base::Vector<Vec3> pushers;
  const ecs::Entity local =
      player_actor_ >= 0 ? actors_[player_actor_].entity : ecs::kInvalidEntity;
  if (world_.IsAlive(local))
    if (const world::Transform* t = world_.Get<world::Transform>(local))
      pushers.push_back({t->position[0], t->position[1], t->position[2]});
  world_.Each<net::NetworkId, world::Transform>(
      [&](ecs::Entity, net::NetworkId&, world::Transform& t) {
        pushers.push_back({t.position[0], t.position[1], t.position[2]});
      });
  if (pushers.empty()) return;

  constexpr f32 kPushRadius = 0.6f;  // ~capsule radius in meters
  world_.Each<world::Npc, world::Transform>([&](ecs::Entity, world::Npc&, world::Transform& nt) {
    for (const Vec3& p : pushers) {
      const float pusher[3] = {p.x, p.y, p.z};
      float out[3];
      if (world::ShoveOutOfRadius(pusher, nt.position, kPushRadius, out)) {
        nt.position[0] = out[0];
        nt.position[1] = out[1];
        nt.position[2] = out[2];
      }
    }
  });
}

bool Engine::RunFrame() {
  if (quit_.load(std::memory_order_relaxed)) return false;
  if (window_ && !window_->PumpEvents()) return false;
  {
    int steps = timer_.Tick();
    f32 dt = static_cast<f32>(timer_.fixed_step());
    // Place NPC / other-player collision capsules at their current transforms
    // before the sim step, so the player's character controller collides with
    // them where they are this frame.
    SyncSolidBodies();
    for (int i = 0; i < steps; ++i) {
      scheduler_.RunStage(ecs::Stage::kPreSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kPostSim, world_, dt);
    }

    // The guest advances on the main loop's clock; it does its work on its own
    // thread, so this only posts a tick.
    if (scripts_) scripts_->Tick(static_cast<f32>(timer_.frame_delta()));

    // Apply (and, when hosting, replicate) the world mutations quests requested
    // on the guest thread. Main-thread only, so it owns the ECS exclusively here.
    ApplyQuestWorld();

    // Authoritative NPC simulation runs on the host / single-player only; a
    // client receives the results via actor sync instead of simulating.
    if (!client_session_) ServerSimulateActors(static_cast<f32>(timer_.frame_delta()));

    if (!config_.headless) {
      f32 frame_delta = static_cast<f32>(timer_.frame_delta());
      debug_ui_.BeginFrame();
      UpdateCamera(frame_delta);
      SyncNpcActors();  // add/remove NPC actors as cells stream in/out
      UpdateActors(frame_delta);
      scheduler_.RunStage(ecs::Stage::kPreRender, world_, frame_delta);

      render::FrameView view;
      if (walk_mode_ && player_actor_ >= 0) {
        view.camera.eye = walk_eye_;
        view.camera.target = walk_target_;
      } else {
        view.camera.eye = camera_.position();
        view.camera.target = camera_.target();
      }
      view.frame_delta_seconds = frame_delta;
      // Rebuilt every frame so destroyed entities drop out on their own.
      base::UnorderedMap<u64, Mat4> transforms;
      world_.Each<world::Transform, world::Renderable>(
          [&](ecs::Entity entity, world::Transform& transform, world::Renderable& renderable) {
            if (world_.Has<world::Hidden>(entity)) return;  // Disable()d by a quest
            u64 key = static_cast<u64>(entity.generation) << 32 | entity.index;
            Mat4 current = TransformMatrix(transform);
            const Mat4* prev = prev_transforms_.find(key);
            view.draws.push_back({renderable.mesh.hash, current, prev ? *prev : current});
            transforms.insert(key, current);
          });
      prev_transforms_ = std::move(transforms);
      EmitActorDraws(view);
      UpdateParticles(frame_delta, view);
      if (gpu_particle_count_ > 0) {
        view.gpu_particle_count = gpu_particle_count_;
        view.gpu_particle_emitter = gpu_particle_emitter_;
      }
      if (fur_ball_) {
        view.fur_ball = true;
        view.fur_position = fur_position_;
      }
      if (!oit_instances_.empty()) view.oit = oit_instances_;
      if (!demo_gaussians_.empty()) view.gaussians = demo_gaussians_;
      if (!demo_lights_.empty()) view.lights = demo_lights_;
      RefreshQuestPanel(frame_delta);
      RefreshNativeTrace(frame_delta);
      debug_ui_.Build(renderer_, camera_, frame_delta, &view, &quest_panel_, &native_trace_panel_);
      game_ui_.Build(*window_, renderer_, camera_, frame_delta, &view);
      renderer_.RenderFrame(view);
    } else {
      // No vsync to pace the loop; yield between fixed steps instead of
      // spinning a core.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return !quit_.load(std::memory_order_relaxed);
}

int Engine::Run() {
  while (RunFrame()) {
  }
  return 0;
}

void Engine::OnSurfaceDestroyed() {
  if (!config_.headless) renderer_.DestroySurface();
}

void Engine::OnSurfaceCreated() {
  if (!config_.headless) renderer_.RecreateSurface();
}

Engine::~Engine() { Shutdown(); }

void Engine::Shutdown() {
  if (shut_down_) return;  // idempotent: explicit Shutdown then destructor
  shut_down_ = true;
  // Stop the guest thread before tearing down the systems its bindings touch.
  scripts_.reset();
  if (!config_.headless) {
    renderer_.WaitIdle();
    game_ui_.Shutdown();
    debug_ui_.Shutdown();
    renderer_.Shutdown();
  }
  if (jobs_) jobs_->WaitIdle();
}

}  // namespace rec
