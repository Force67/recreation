#include "engine.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <base/option.h>

#include "asset/primitives.h"
#include "core/feature_registry.h"
#include "core/log.h"
#include "item_bridge.h"
#include "player_controller.h"
#include "world/components.h"

#if defined(RECREATION_HAS_UGUI)
#include <string>

#include "asset/pack.h"
#include "shader_pack.h"
#endif

// Engine lifecycle and service wiring: construction (renderer/UI/physics bringup
// and the shared EngineContext), the resolved render preset, the front-door
// content dispatch, and ordered teardown. The frame loop, networking, content
// loading, the main menu, managed scripting and camera/input live in sibling
// engine translation units (see frame_loop.cc, networking.cc, content_load.cc,
// main_menu.cc, managed_scripting.cc, camera_input.cc).
namespace rx {
namespace {
// SunDir pins a fixed sun for headless lighting/shadow tests (its presence
// disables the world clock driving the sun); the renderer parses the value.
// The window size / timescale / occlusion options are owned by app::Host now
// (RX_WIN_W/H, RX_TIMESCALE, RX_GAME_HOUR, RX_NO_OCCLUSION, RX_FIXED_DT), so the
// game must not re-register them.
base::Option<const char*> SunDir{"sun.dir", nullptr, "RX_SUN_DIR"};

#if defined(RECREATION_HAS_UGUI)
// The recreation-owned HUD/thumbnail shaders live in shaders.rxp (built from
// runtime/shaders). RECREATION_SHADER_PACK overrides the compiled-in path;
// RECREATION_SHADER_DIR points at a loose directory of freshly compiled .spv
// that overrides the pack (later Vfs mounts win) for shader iteration.
base::Option<const char*> ShaderPackOpt{"shader.pack", nullptr, "RECREATION_SHADER_PACK"};
base::Option<const char*> ShaderDirOpt{"shader.dir", nullptr, "RECREATION_SHADER_DIR"};

std::string ShaderPackPath() {
  if (const char* env = ShaderPackOpt.get(); env && *env) return env;
#ifdef RECREATION_SHADER_PACK_DEFAULT
  return RECREATION_SHADER_PACK_DEFAULT;
#else
  return "shaders.rxp";
#endif
}
#endif
}  // namespace

bool Engine::OnInitialize(app::Services& services) {
  // The host has brought the generic subsystems up and resolved the render
  // preset / seeded the day/night clock; cache non-owning views of them.
  host_ = services.host;
  window_ = services.window;
  clock_ = services.clock;
  world_ = services.world;
  scheduler_ = services.scheduler;
  renderer_ = services.renderer;
  physics_ = services.physics;
  vfs_ = services.vfs;
  audio_ = services.audio;
  input_map_ = services.input_map;
  actions_ = services.actions;

#if defined(RECREATION_HAS_UGUI)
  // Mount recreation's compiled HUD/thumbnail shaders under the shaders:// scheme
  // before any pipeline is built, then route shader-blob lookup through the Vfs.
  // Missing/partial archives fall through to the blobs embedded in the binary.
  if (vfs_) {
    const std::string pack = ShaderPackPath();
    if (auto provider = asset::MakePackFileProvider(pack)) {
      vfs_->Mount("shaders", std::move(provider));
      RX_INFO("shaders: mounted {} under shaders://", pack);
    } else {
      RX_WARN("shaders: {} unavailable, using embedded shader blobs", pack);
    }
    if (const char* dir = ShaderDirOpt.get(); dir && *dir) {
      vfs_->Mount("shaders", asset::MakeLooseFileProvider(dir));
      RX_INFO("shaders: loose override dir {} shadows the archive", dir);
    }
    shaderpack::SetVfs(vfs_);
  }
#endif

  // Quest-driven world effects hold a World&, so build them now the world exists.
  quest_world_ = std::make_unique<world::QuestWorld>(*world_);
  // When SunDir is set the world clock stops driving the day/night cycle.
  drive_sun_from_clock_ = SunDir.get() == nullptr;

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, *renderer_)) {
      RX_WARN("debug ui unavailable");
    }
    debug_ui_.set_clock(clock_);  // Lighting panel scrubs the day/night cycle
    // Weather playground: the panel edits the director's override in place, and
    // "Strike now" forces a test bolt ~300 m from the camera.
    debug_ui_.set_weather(director_.debug_override_enable(), director_.debug_override_state(),
                          [this] { director_.RequestStrike(300.0f); });
    if (!game_ui_.Initialize(*window_, *renderer_)) {
      RX_WARN("game ui unavailable");
    }
  }

  // Load the user's controls config (key/pad bindings, look sensitivity, invert,
  // lightbar) into the host's input map and apply it; falls back to built-in
  // defaults when absent.
  LoadControls();

  // Wire the shared service bundle and build the gameplay subsystems. The
  // late-built services (assets/streamer/scripts/bindings/net) are filled into
  // the context as they come up in LoadGameData / StartNetworking.
  ctx_.config = &config_;
  ctx_.world = world_;
  ctx_.scheduler = scheduler_;
  ctx_.renderer = renderer_;
  ctx_.camera = &camera_;
  ctx_.physics = physics_;
  ctx_.vfs = vfs_;
  ctx_.records = &records_;
  ctx_.strings = &strings_;
  ctx_.dialogue = &dialogue_;
  ctx_.quest_world = quest_world_.get();
  ctx_.debug_ui = &debug_ui_;
  ctx_.game_ui = &game_ui_;
  ctx_.physics_entities = &physics_entities_;
  ctx_.audio = audio_;
  ctx_.actions = actions_;  // resolved input for the first-person equip key
  actors_ = std::make_unique<ActorSystem>(ctx_);
  interaction_ = std::make_unique<InteractionSystem>(ctx_, actors_.get());
  items_ = std::make_unique<ItemBridge>(ctx_, actors_.get());
  ctx_.items = items_.get();
  npc_ = std::make_unique<NpcDirector>(ctx_, actors_.get());
  quest_ = std::make_unique<QuestDirector>(ctx_, actors_.get());
  demos_ = std::make_unique<DemoScenes>(ctx_, actors_.get());
  npc_->set_siblings(interaction_.get(), quest_.get());
  quest_->set_siblings(npc_.get(), interaction_.get());
  // The live map editor (windowed client only). Constructed after game_ui_ is up
  // so it can register its overlay event sink; ticked from UpdateCamera.
  if (!config_.headless) editor_ = std::make_unique<MapEditor>(ctx_);
  // Character creation (RX_CHARGEN); Enter() runs once game data has loaded.
  if (!config_.headless) chargen_ = std::make_unique<CharGen>(ctx_);

  // Place NPC / other-player collision capsules at their current transforms
  // before each sim step, so the player's character controller collides with
  // them where they are this frame.
  scheduler_->AddSystem(ecs::Stage::kPreSim, "sync_solid_bodies",
                        [this](ecs::World&, f32) { actors_->SyncSolidBodies(); });

  if (physics_->initialized()) {
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
      renderer_->UploadMaterial(wood);
      renderer_->UploadMesh(cube);
    }
    // The host steps physics_ (physics_.Update) in its own kSim system; this one,
    // registered after it, only mirrors our dynamic bodies into their world::
    // Transform proxies (the host's mirror targets scene::Transform, which the
    // game does not use).
    scheduler_->AddSystem(ecs::Stage::kSim, "physics_mirror", [this](ecs::World& world, f32) {
      for (const PhysicsEntity& body : physics_entities_) {
        world::Transform* transform = world.Get<world::Transform>(body.entity);
        if (!transform) continue;
        Vec3 position;
        f32 rotation[4];
        if (physics_->GetBodyTransform(body.body, &position, rotation)) {
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

  scheduler_->AddSystem(ecs::Stage::kPostSim, "cell_streaming", [this](ecs::World& world, f32) {
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

Engine::~Engine() = default;

void Engine::OnShutdown() {
  // Called by the host while the renderer is idle but still alive, and after the
  // host has already stopped the audio device. The host owns the renderer/jobs
  // teardown; here the game drops its own state in the order its threads need.
  SaveControls();  // persist any in-session rebinds / sensitivity changes
  if (items_) items_->Save();  // persist inventory + world items + removed refs
  // Run managed teardown while the guest is still alive (its shutdown callbacks
  // dispatch through the bridge), then stop the guest so no more events reach the
  // host, then destroy the host. This exact order keeps the event sink, which
  // the guest thread holds, valid until the guest is joined.
  if (managed_) managed_->Shutdown();
  scripts_.reset();
  extra_streamers_.clear();  // reference domain records/assets; drop before the domains
  extra_domains_.clear();    // joins each secondary guest thread before teardown
  managed_.reset();
  // GPU-dependent UI resources, released while the renderer is still alive.
  if (!config_.headless) {
    game_ui_.Shutdown();
    debug_ui_.Shutdown();
  }
#if RECREATION_HAS_NET
  bubble_viz_.reset();  // owns a raw pipeline; drop it while the device lives
#endif
}

}  // namespace rx
