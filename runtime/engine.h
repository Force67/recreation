#ifndef RECREATION_RUNTIME_ENGINE_H_
#define RECREATION_RUNTIME_ENGINE_H_

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "anim/foot_ik.h"
#include "anim/locomotion.h"
#include "anim/pose.h"
#include "asset/asset_database.h"
#include "asset/skeleton.h"
#include "asset/vfs.h"
#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "core/frame_timer.h"
#include "core/job_system.h"
#include "core/window.h"
#include "debug_ui.h"
#include "fly_camera.h"
#include "game_ui.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "net/session.h"
#include "physics/physics_world.h"
#include "render/presets.h"
#include "render/renderer.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/script_system.h"
#include "world/cell_streaming.h"

namespace rec {

struct EngineConfig {
  std::string data_dir;
  std::string plugins_txt;
  std::string gltf_path;  // standalone gltf/glb scene (e.g. sponza)
  std::string demo_scene;  // "water" = empty map with a water sheet
  bethesda::Game game = bethesda::Game::kUnknown;  // kUnknown = autodetect
  // Exterior cell the camera starts in. The default is the Whiterun plains.
  i32 start_cell_x = 5;
  i32 start_cell_y = -3;
  // Interior cell to load instead of streaming the exterior worldspace.
  // Editor id ("WhiterunBanneredMare") or a hex load order form id ("0x...").
  std::string interior;
  f32 grass_density = 1.0f;  // multiplies every GRAS density, 0 disables
  // Cap on quest scripts instantiated at load (0 = all). The quest browser
  // lists what's attached; the default attaches every scripted quest.
  int max_quest_scripts = 0;
  render::RendererDesc renderer;
  // Hardware quality tier. kAuto picks one from the gpu at startup; the rest
  // force a tier (steam deck, android, low/medium/high/ultra, console).
  render::QualityPreset preset = render::QualityPreset::kAuto;
  bool headless = false;
  bool host_server = false;
  u16 port = 29700;
  std::string connect_address;
  std::string player_name = "player";
  u32 max_clients = 64;
};

class Engine {
 public:
  bool Initialize(const EngineConfig& config);
  int Run();
  void Shutdown();

  // Safe to call from a signal handler; Run() returns after the current
  // frame.
  void RequestQuit() { quit_.store(true, std::memory_order_relaxed); }

 private:
  // Skinned, animated characters. Each owns a skeleton + pose; its parts are
  // skinned meshes sharing that pose. Kept engine-side (not ECS components)
  // because the renderer needs the CPU skin bindings to build palettes.
  struct ActorPart {
    asset::AssetId mesh;
    asset::SkinBinding skin;
    base::Vector<i32> remap;  // skin bone -> skeleton bone index
    // Rigid parts (head, hair) are not skinned: they ride a single bone.
    // attach_bone >= 0 selects it; the part follows bone_model * inverse bind.
    i32 attach_bone = -1;
    Mat4 attach_inverse_bind = Mat4::Identity();
  };
  struct Actor {
    ecs::Entity entity;
    asset::Skeleton skeleton;
    anim::Locomotion locomotion;
    anim::SkeletonPose pose;
    base::Vector<Mat4> bone_model;     // model-space per skeleton bone
    base::Vector<ActorPart> parts;
    bool animate = true;               // false = hold the bind pose
    f32 speed = 0;                     // planar speed feeding the gait
    Mat4 skeleton_to_local = Mat4::Identity();  // skeleton space -> entity local
    Mat4 prev_model = Mat4::Identity();
    // Foot IK config, in model space. up/forward are the ground-up and facing
    // axes (engine rig: +Y/+Z; Skyrim: +Z/+Y).
    bool foot_ik = false;
    Vec3 ik_up{0, 1, 0};
    Vec3 ik_forward{0, 0, 1};
    f32 ankle_height = 0.02f;
    // Walk mode: a Jolt character capsule drives the entity; yaw faces movement.
    physics::CharacterId character = 0;
    f32 yaw = 0;             // facing, radians about engine up (+Y)
    f32 capsule_offset = 0;  // entity origin to capsule centre, along up
  };

  bool LoadGameData();
  bool LoadInterior();
  void MountArchives();
  // Instantiates the Papyrus scripts attached to quest records (bounded), so a
  // slice of the game's scripts run live inside the engine.
  void AttachQuestScripts();
  // Refreshes the debug overlay's quest snapshot (throttled) and wires its
  // start/stop/stage callbacks to the guest thread.
  void RefreshQuestPanel(f32 dt);
  // Enables guest native-call tracing while the trace window is open and
  // snapshots its ring into the overlay (throttled).
  void RefreshNativeTrace(f32 dt);
  bool StartNetworking();
  // Resolves the configured quality tier from the gpu (or a forced preset) and
  // applies it to the renderer's live settings.
  void ApplyRenderPreset();
  void CreateDemoScene();
  void CreateWaterDemoScene();
  // Sphere grid sweeping the extended pbr lobes (clearcoat/anisotropy/sheen).
  void CreateMaterialDemoScene();
  // A sphere built from 3D gaussian splats (non-triangle primitive path).
  void CreateGaussianDemoScene();
  // A receding row of identical multi-lod spheres; the gpu picks coarser
  // tessellation with distance. Exercises distance-based lod selection.
  void CreateLodDemoScene();
  // A Cornell box (red/green side walls): the canonical color-bleed test for
  // global illumination. Shows ssgi (raster) or ddgi (rt) bouncing wall color.
  void CreateCornellDemoScene();
  // A gpu-simulated ember fountain (compute), scaled past what the cpu sim can.
  void CreateGpuParticleDemoScene();
  // A shell-fur ball: the hair/fur rendering technique on a sphere.
  void CreateFurDemoScene();
  // A high-poly sphere with auto-generated (decimated) lods, instanced at
  // increasing distance so the simplifier's coarser lods read with range.
  void CreateAutoLodDemoScene();
  // A row of spheres, each shaded by a MaterialX (.mtlx) file from REC_MTLX.
  void CreateMaterialXDemoScene();
  // Overlapping transparent spheres rendered with weighted-blended OIT.
  void CreateOitDemoScene();

  // A simple cpu particle fountain for the demos. Integrates + spawns each
  // frame and emits the live billboards into the frame view.
  struct DemoParticle {
    Vec3 position;
    Vec3 velocity;
    f32 life = 0;
    f32 max_life = 1;
    f32 size = 0.1f;
    Vec3 color;
  };
  void UpdateParticles(f32 dt, render::FrameView& view);
  void CreateTestCharacter();
  // Assembles an animated actor from real Skyrim data: loads skeleton.nif into
  // an engine Skeleton and the worn body-part NIFs as skinned meshes bound to
  // it by bone name. Returns false if the core skeleton/body could not load.
  bool CreateSkyrimActor();
  // Loads one body-part NIF, uploads it, and appends it to `actor`. Skinned
  // parts bind to the skeleton by name; a part with no skin falls back to a
  // rigid mesh riding `attach_bone` (>= 0), used for the head and hair.
  bool LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone = -1);
  // Collects up to `max` non-female HDPT model paths of a head-part type
  // (3 = hair, 1 = face), for assembling an actor's head.
  base::Vector<std::string> FindHeadPartModels(u32 part_type, u32 max);
  void ThrowPhysicsCube();
  bool LoadGltfScene();
  void UpdateCamera(f32 frame_delta);
  // Camera record/replay (deterministic playback for benchmarks and capture).
  // REC_ORBIT turntables the camera, REC_RECORD=<path> writes the path each
  // frame, REC_REPLAY=<path> drives the camera from a recorded path.
  void DriveCamera(f32 dt);
  void LookCameraAt(const Vec3& eye, const Vec3& center);
  // Walk mode step: input -> character move -> entity transform + follow camera.
  void WalkUpdate(f32 dt, bool allow_input);
  // Advances every actor's gait and recomputes its model-space bone matrices.
  void UpdateActors(f32 dt);
  // Appends each actor's skinned draws + bone palettes to the frame view.
  void EmitActorDraws(render::FrameView& view);

  bool particles_enabled_ = false;
  Vec3 particle_emitter_{0, 0, 0};
  u32 gpu_particle_count_ = 0;  // > 0 selects the gpu-simulated fountain
  Vec3 gpu_particle_emitter_{0, 0, 0};
  bool fur_ball_ = false;
  Vec3 fur_position_{0, 0, 0};
  base::Vector<render::WboitInstance> oit_instances_;
  base::Vector<DemoParticle> demo_particles_;
  base::Vector<render::GaussianInstance> demo_gaussians_;
  u32 particle_seed_ = 0x9e3779b9u;
  f32 particle_spawn_accum_ = 0;

  EngineConfig config_;
  bethesda::Game game_ = bethesda::Game::kUnknown;

  std::unique_ptr<Window> window_;
  std::unique_ptr<JobSystem> jobs_;
  FrameTimer timer_;

  ecs::World world_;
  ecs::Scheduler scheduler_;

  asset::Vfs vfs_;
  std::unique_ptr<asset::AssetDatabase> assets_;
  bethesda::RecordStore records_;
  std::unique_ptr<world::CellStreamer> streamer_;
  // Declared before scripts_ so the guest thread (which calls into the bindings)
  // is joined in ScriptSystem's destructor before the bindings are torn down.
  std::unique_ptr<rec::script::skyrim::RecordBackedSkyrimBindings> script_bindings_;
  std::unique_ptr<rec::script::ScriptSystem> scripts_;

  render::Renderer renderer_;
  FlyCamera camera_;

  // Camera record/replay state, lazily armed from env on the first frame.
  struct CamKey {
    f32 t = 0;
    Vec3 pos{};
    Vec3 target{};
  };
  bool cam_init_ = false;
  bool cam_orbit_ = false;
  f32 cam_time_ = 0;
  std::FILE* cam_record_ = nullptr;
  base::Vector<CamKey> cam_replay_;
  DebugUi debug_ui_;
  // Quest console: the (handle, name) of each attached quest, plus the debug
  // overlay's live snapshot/callbacks. Refreshed on a timer off the guest.
  base::Vector<std::pair<u64, std::string>> quest_records_;
  QuestPanel quest_panel_;
  f32 quest_ui_timer_ = 0;
  // Native-call trace window state (F2): the overlay snapshot, refresh timer,
  // and whether guest tracing is currently enabled.
  NativeTracePanel native_trace_panel_;
  f32 trace_ui_timer_ = 0;
  bool native_trace_on_ = false;
  GameUi game_ui_;
  physics::PhysicsWorld physics_;
  // Dynamic bodies mirrored into ECS transforms after each step.
  struct PhysicsEntity {
    physics::BodyId body;
    ecs::Entity entity;
  };
  base::Vector<PhysicsEntity> physics_entities_;
  asset::AssetId physics_cube_mesh_;

  base::Vector<Actor> actors_;
  i32 player_actor_ = -1;  // index into actors_ the walk mode drives, -1 = none

  // Walk-on-the-map mode: WASD drives the player actor, mouse looks, C swaps
  // first/third person. Toggled with T; otherwise the fly camera roams.
  bool walk_mode_ = false;
  bool third_person_ = true;
  bool auto_walk_ = false;  // REC_AUTOWALK: force walk mode + forward for testing
  f32 cam_yaw_ = 0;
  f32 cam_pitch_ = -0.15f;
  Vec3 walk_eye_{};
  Vec3 walk_target_{};
  // Last frame's world matrices keyed by entity, for motion vectors.
  base::UnorderedMap<u64, Mat4> prev_transforms_;
  std::unique_ptr<net::Session> session_;
  // Typed views into session_, null unless that role is active.
  net::ServerSession* server_session_ = nullptr;
  net::ClientSession* client_session_ = nullptr;
  f32 demo_input_time_ = 0;

  std::atomic<bool> quit_ = false;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_ENGINE_H_
