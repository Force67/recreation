#include "engine.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <base/option.h>

#include "asset/primitives.h"
#include "core/feature_registry.h"
#include "core/log.h"
#include "world/components.h"

// Engine lifecycle and service wiring: construction (renderer/UI/physics bringup
// and the shared EngineContext), the resolved render preset, the front-door
// content dispatch, and ordered teardown. The frame loop, networking, content
// loading, the main menu, managed scripting and camera/input live in sibling
// engine translation units (see frame_loop.cc, networking.cc, content_load.cc,
// main_menu.cc, managed_scripting.cc, camera_input.cc).
namespace rec {
namespace {
// Engine-startup options. Namespace scope, so they register before
// InitOptionsFromEnv() runs. WinW/WinH=0 keep the WindowDesc default
// (1920x1080); they shrink the window for fast headless capture (e.g. the
// software-rendered swrun path).
base::Option<int> WinW{"win.width", 0, "REC_WIN_W"};
base::Option<int> WinH{"win.height", 0, "REC_WIN_H"};
// SunDir pins a fixed sun for headless lighting/shadow tests (its presence
// disables the world clock driving the sun); the renderer parses the value.
base::Option<const char*> SunDir{"sun.dir", nullptr, "REC_SUN_DIR"};
base::Option<bool> NoOcclusion{"no.occlusion", false, "REC_NO_OCCLUSION"};
// Timescale (0 freezes time) overrides the game's timescale; GameHour overrides
// the mid-morning start the world boots lit at.
base::Option<float> Timescale{"timescale", 0.0f, "REC_TIMESCALE"};
base::Option<float> GameHour{"game.hour", 11.0f, "REC_GAME_HOUR"};
}  // namespace

bool Engine::Initialize(const EngineConfig& config, std::unique_ptr<Window> window) {
  config_ = config;
  InitFeatures();              // apply REC_FEATURES overrides before any flag read
  base::InitOptionsFromEnv();  // populate every base::Option from the environment
  jobs_ = std::make_unique<JobSystem>();
  // When SunDir is set the world clock stops driving the day/night cycle.
  // Seed the clock now so the demo and glTF scenes (which never load game data)
  // still get a lit time of day; LoadGameData reseeds it with the game's
  // authored timescale.
  drive_sun_from_clock_ = SunDir.get() == nullptr;
  ConfigureClock(20.0f);

  if (!config_.headless) {
    WindowDesc desc;
    if (WinW > 0) desc.width = static_cast<u32>(WinW.get());
    if (WinH > 0) desc.height = static_cast<u32>(WinH.get());
    window_ = window ? std::move(window) : Window::Create(desc);
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
    ApplyRenderPreset();
  }

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("debug ui unavailable");
    }
    debug_ui_.set_clock(&clock_);  // Lighting panel scrubs the day/night cycle
    debug_ui_.set_weather(&weather_override_, &weather_override_state_);  // Weather playground
    if (!game_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("game ui unavailable");
    }
  }

  // Load the user's controls config (key/pad bindings, look sensitivity, invert,
  // lightbar) and apply it; falls back to built-in defaults when absent.
  LoadControls();

  // Wire the shared service bundle and build the gameplay subsystems. The
  // late-built services (assets/streamer/scripts/bindings/net) are filled into
  // the context as they come up in LoadGameData / StartNetworking.
  ctx_.config = &config_;
  ctx_.world = &world_;
  ctx_.scheduler = &scheduler_;
  ctx_.renderer = &renderer_;
  ctx_.camera = &camera_;
  ctx_.physics = &physics_;
  ctx_.vfs = &vfs_;
  ctx_.records = &records_;
  ctx_.strings = &strings_;
  ctx_.dialogue = &dialogue_;
  ctx_.quest_world = &quest_world_;
  ctx_.debug_ui = &debug_ui_;
  ctx_.game_ui = &game_ui_;
  ctx_.physics_entities = &physics_entities_;
  // Audio comes up before content loads (it reads sound bytes lazily through the
  // Vfs). Headless servers and mute (REC_AUDIO_MUTE) open no device and run
  // silent; the rest of the engine is unaffected either way.
  audio_ = std::make_unique<audio::AudioSystem>();
  audio_->Initialize(&vfs_);
  ctx_.audio = audio_.get();
  actors_ = std::make_unique<ActorSystem>(ctx_);
  interaction_ = std::make_unique<InteractionSystem>(ctx_, actors_.get());
  npc_ = std::make_unique<NpcDirector>(ctx_, actors_.get());
  quest_ = std::make_unique<QuestDirector>(ctx_, actors_.get());
  demos_ = std::make_unique<DemoScenes>(ctx_, actors_.get());
  npc_->set_siblings(interaction_.get(), quest_.get());
  quest_->set_siblings(npc_.get(), interaction_.get());
  // The live map editor (windowed client only). Constructed after game_ui_ is up
  // so it can register its overlay event sink; ticked from UpdateCamera.
  if (!config_.headless) editor_ = std::make_unique<MapEditor>(ctx_);

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

  // A bare windowed launch (no content source, no networking role) opens the
  // NEXUS main menu as the front door.
  if (!config_.main_menu && config_.data_dir.empty() && config_.gltf_path.empty() &&
      config_.demo_scene.empty() && !config_.headless && !config_.host_server &&
      config_.connect_address.empty()) {
    config_.main_menu = true;
  }

  if (config_.main_menu && !config_.headless) {
    LoadSetupConfig(*this);  // fold any persisted game paths / mods dir into config
    if (FirstRunComplete())
      SetupMainMenu(*this);  // pick a universe; the game loads on demand (EnterUniverse)
    else
      SetupFirstRun(*this);  // fresh install: run the out-of-box setup wizard first
  } else if (!config_.gltf_path.empty()) {
    if (!LoadGltfScene()) return false;
  } else if (!config_.data_dir.empty()) {
    if (!LoadGameData(*this)) return false;
  } else {
    demos_->CreateDemoScene();
  }

#if RECREATION_HAS_NET
  // In menu mode the session is opened later, when a universe is entered.
  if (!config_.main_menu && !StartNetworking(*this)) return false;
#endif

  scheduler_.AddSystem(ecs::Stage::kPostSim, "cell_streaming", [this](ecs::World& world, f32) {
    if (!streamer_) return;
    // In walk mode the streamer follows the player, not the fly camera (which is
    // frozen while walking): this keeps cells loaded as the player walks far and,
    // crucially, as quest fragments and load doors teleport them across the
    // worldspace, instead of leaving them stranded in unstreamed space.
    Vec3 anchor = camera_.position();
    Vec3 ppos;
    if (ctx_.walk_mode && actors_->PlayerWorldPos(&ppos)) anchor = ppos;
    // Multi-game trailer: only the active game streams (around the shared center),
    // so the maps never all sit resident at once. SwitchTrailerDomain unloads the
    // others as the trailer cuts between them.
    if (trailer_sequential_) {
      if (world::CellStreamer* active = TrailerStreamer(trailer_active_domain_))
        active->Update(world, anchor);
      return;
    }
    streamer_->Update(world, anchor);
    // Secondary worldspaces follow the same anchor; each applies its own offset
    // internally so it streams the region that lands beside the primary world.
    for (auto& extra : extra_streamers_) extra->Update(world, anchor);
  });

  return true;
}

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
  // Carry the path-tracer mode + tunables (REC_PATHTRACE_RECON / _REFERENCE / _SPP
  // / _ACCUM ...) through the preset, or env-selected recon/reference silently
  // falls back to the NRD path.
  tuned.path_trace_reference = env.path_trace_reference;
  tuned.path_trace_recon = env.path_trace_recon;
  tuned.path_trace_spp = env.path_trace_spp;
  tuned.path_trace_accum = env.path_trace_accum;
  tuned.path_trace_recon_weight = env.path_trace_recon_weight;
  tuned.path_trace_recon_atrous = env.path_trace_recon_atrous;
  tuned.path_trace_recon_debug = env.path_trace_recon_debug;
  tuned.path_trace_restir = env.path_trace_restir;
  tuned.path_trace_restir_di = env.path_trace_restir_di;
  tuned.hdr_output = env.hdr_output;
  tuned.hdr_paper_white = env.hdr_paper_white;
  tuned.path_trace_rr = env.path_trace_rr;
  if (env.wireframe) tuned.wireframe = true;  // honor REC_WIREFRAME over the preset
  tuned.ssr = env.ssr;                        // honor REC_SSR over the preset
  tuned.ssgi = env.ssgi;                      // honor REC_SSGI over the preset
  tuned.color_grade = env.color_grade;        // presets never set a grade
  tuned.sun_direction = env.sun_direction;    // honor REC_SUN_DIR over the default
  // Sky/weather env overrides (REC_AERIAL / REC_CLOUDS / REC_CLOUD_COVERAGE /
  // REC_PRECIP / REC_SNOW), so they survive the preset. A loaded game's weather
  // re-drives these per frame; this keeps them working in the demo/glTF scenes.
  tuned.fog = env.fog;  // honor REC_FOG over the preset (fog params are defaults)
  tuned.motion_blur = env.motion_blur;  // honor REC_MOTION_BLUR over the preset
  tuned.dof = env.dof;
  tuned.dof_focus = env.dof_focus;
  tuned.dof_aperture = env.dof_aperture;
  tuned.aerial_perspective = env.aerial_perspective;
  tuned.clouds = env.clouds;
  tuned.cloud_coverage = env.cloud_coverage;
  tuned.precipitation = env.precipitation;
  tuned.precip_snow = env.precip_snow;
  tuned.aurora = env.aurora;
  if (NoOcclusion) tuned.gpu_occlusion = false;  // a/b baseline

  renderer_.settings() = tuned;
  REC_INFO("render preset: {} ({})", render::PresetName(resolved),
           config_.preset == render::QualityPreset::kAuto ? "auto" : "forced");
}

void Engine::ConfigureClock(f32 base_timescale) {
  f32 timescale = base_timescale > 0 ? base_timescale : 20.0f;
  if (Timescale.overridden() && Timescale.get() >= 0) timescale = Timescale.get();
  const f32 start_hour = GameHour.get();
  clock_.Configure(start_hour, timescale);
  REC_INFO("day/night clock: start hour {:.1f}, timescale {:.0f}", start_hour, timescale);
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
  SaveControls();  // persist any in-session rebinds / sensitivity changes
  // Stop the audio device thread early, before the systems whose sounds it might
  // still be streaming go away.
  if (audio_) audio_->Shutdown();
  // Run managed teardown while the guest is still alive (its shutdown callbacks
  // dispatch through the bridge), then stop the guest so no more events reach the
  // host, then destroy the host. This exact order keeps the event sink, which
  // the guest thread holds, valid until the guest is joined.
  if (managed_) managed_->Shutdown();
  scripts_.reset();
  extra_streamers_.clear();  // reference domain records/assets; drop before the domains
  extra_domains_.clear();    // joins each secondary guest thread before teardown
  managed_.reset();
  if (!config_.headless) {
    renderer_.WaitIdle();
    game_ui_.Shutdown();
    debug_ui_.Shutdown();
    renderer_.Shutdown();
  }
  if (jobs_) jobs_->WaitIdle();
}

}  // namespace rec
