#ifndef RECREATION_RUNTIME_ENGINE_H_
#define RECREATION_RUNTIME_ENGINE_H_

#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
#include "bethesda/strings.h"
#include "core/frame_timer.h"
#include "dialogue/dialogue.h"
#include "core/job_system.h"
#include "core/window.h"
#include "debug_ui.h"
#include "fly_camera.h"
#include "game_ui.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#if RECREATION_HAS_NET
#include "net/session.h"
#endif
#include "physics/physics_world.h"
#include "quest/quest_system.h"
#include "quest/scene.h"
#include "render/presets.h"
#include "render/renderer.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/script_system.h"
#include "world/cell_streaming.h"
#include "world/quest_world.h"

namespace rec {

// WorldEffectSink implementation: the Skyrim bindings call this on the guest
// thread; it allocates handles and marshals each mutation into the thread-safe
// WorldCommandQueue, which the main thread drains into QuestWorld. Kept tiny and
// header-only so the script module need not know about the ECS.
class RuntimeWorldSink : public script::WorldEffectSink {
 public:
  explicit RuntimeWorldSink(world::WorldCommandQueue* queue) : queue_(queue) {}

  u64 SpawnReference(u64 quest, u64 base, f32 x, f32 y, f32 z) override {
    // Synthetic runtime handle in the reserved 0xFFFF plugin slot, so it never
    // collides with a real form id; allocated here so PlaceAtMe can return it.
    const u64 handle = (0xFFFFull << 32) | next_handle_.fetch_add(1);
    world::WorldCommand c;
    c.op = world::WorldOp::kSpawn;
    c.quest = quest;
    c.handle = handle;
    c.base = base;
    c.pos = {x, y, z};
    queue_->Push(c);
    return handle;
  }
  void MoveReference(u64 quest, u64 handle, f32 x, f32 y, f32 z) override {
    Emit(world::WorldOp::kMove, quest, handle, x, y, z);
  }
  void MovePlayer(u64 quest, f32 x, f32 y, f32 z) override {
    Emit(world::WorldOp::kMovePlayer, quest, 0, x, y, z);
  }
  void SetEnabled(u64 quest, u64 handle, bool enabled) override {
    world::WorldCommand c;
    c.op = world::WorldOp::kSetEnabled;
    c.quest = quest;
    c.handle = handle;
    c.enabled = enabled;
    queue_->Push(c);
  }
  void DeleteReference(u64 quest, u64 handle) override {
    Emit(world::WorldOp::kDelete, quest, handle, 0, 0, 0);
  }
  void CleanupQuest(u64 quest) override {
    world::WorldCommand c;
    c.op = world::WorldOp::kCleanupQuest;
    c.quest = quest;
    queue_->Push(c);
  }

 private:
  void Emit(world::WorldOp op, u64 quest, u64 handle, f32 x, f32 y, f32 z) {
    world::WorldCommand c;
    c.op = op;
    c.quest = quest;
    c.handle = handle;
    c.pos = {x, y, z};
    queue_->Push(c);
  }

  world::WorldCommandQueue* queue_;
  std::atomic<u32> next_handle_{1};
};

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
  Engine() = default;
  ~Engine();

  // `window` lets a platform supply its own surface (Android hands the engine
  // the activity's ANativeWindow); when null the engine creates one itself. A
  // failed Initialize tears down whatever it had brought up (the destructor
  // calls Shutdown), so callers need not Shutdown after a failure.
  bool Initialize(const EngineConfig& config, std::unique_ptr<Window> window = nullptr);
  int Run();
  // One iteration of the main loop. Returns false when the engine wants to
  // stop. Platforms that own the loop (Android's activity) drive this directly
  // instead of the blocking Run().
  bool RunFrame();
  void Shutdown();

  // Android lifecycle: the activity's presentation surface is lost when its
  // window goes away (background) and rebound when it returns. The platform
  // entry drives these around RunFrame; the window must already point at the
  // new ANativeWindow before OnSurfaceCreated.
  void OnSurfaceDestroyed();
  void OnSurfaceCreated();

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

  // One selectable line in an open conversation, plus the INFO fragment that
  // runs (server-authoritatively) when it is chosen.
  struct DialogueOption {
    std::string player_line;
    std::string npc_line;
    u64 info = 0;
    u64 quest = 0;
    std::string fragment_function;
  };
  // The conversation the player has open with an NPC. Built on the guest thread
  // (condition-gated) when an NPC is activated; empty/closed otherwise.
  struct DialogueSession {
    bool open = false;
    u64 npc = 0;
    std::string speaker;
    std::string npc_line;
    std::vector<DialogueOption> options;
  };

  // A world waypoint for one quest objective: where the objective sends the
  // player and the stage reaching it advances to. Armed only while its objective
  // is the tracked quest's current displayed-and-incomplete one; entering
  // `radius` (any player, host authoritative) fires SetStage(advance_stage) once.
  // Authored at runtime from the debugger or seeded for a quest.
  struct QuestMarker {
    u64 quest = 0;
    i32 objective = 0;
    i32 advance_stage = -1;  // <0: marker is display-only, never triggers
    Vec3 pos{};
    f32 radius = 2.5f;
    bool fired = false;      // a trigger fires once; cleared by re-authoring
    bool always_arm = false;  // armed regardless of which objective is current
  };

  // SceneSink over the running engine: a scene guides NPCs (steered targets),
  // runs INFO fragments, advances quest stages, and answers proximity queries.
  // Nested so it can reach the engine's private steering/quest state via `e`.
  struct Mq101Sink : public quest::SceneSink {
    Engine* e = nullptr;
    void GuideTo(u64 actor, const float pos[3]) override;
    void SayInfo(u64 actor, u64 info) override;
    void SetStage(u64 quest, i32 stage) override;
    bool ActorAt(u64 actor, const float pos[3], float radius) override;
    bool PlayerNear(const float pos[3], float radius) override;
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
  // Pushes the running-quest snapshot to the HUD tracker: title, objectives,
  // and the "quest updated" banner when the tracked quest changes.
  void UpdateQuestHud(const std::vector<quest::QuestStatus>& running);
  // Objective waypoints: arms the marker for the tracked quest's current
  // objective, drives the HUD compass marker + distance from it, and (host
  // authoritative) advances the quest's stage when a player reaches the marker.
  // The stage advance runs on the guest and replicates like any other, so a
  // client progresses by walking into the same trigger the host evaluates.
  void UpdateObjectiveMarkers(const std::vector<quest::QuestStatus>& running);
  // Drives the HUD compass pip + distance for a marker at world `pos`, computed
  // from this peer's own camera. Shared by the host (its armed marker) and a
  // multiplayer client (the host's replicated marker).
  void DriveObjectiveMarkerHud(bool active, const Vec3& pos);
  // Steers every registered follower NPC toward its formation slot behind the
  // player and writes the resulting transform (host authoritative; the motion
  // streams to clients via actor sync). A no-op with no followers.
  void UpdateFollowers(f32 dt);
  // Registers / clears an NPC (by form handle) as a follower of the player.
  void SetFollower(u64 npc, bool follow);
  // Picks a travel direction toward `goal_dir` that steers around nearby
  // obstacles: fans candidate directions, raycasts each for clearance, and
  // returns the clearest still-goal-ish one in `out_dir`. Falls back to
  // `goal_dir` when physics is unavailable.
  void AvoidObstacles(const float self_pos[3], const float goal_dir[3], float out_dir[3]);
  // Steers one NPC (the actor entity) at `pos`/`rot` (updated in place) toward
  // `goal` with arrival + obstacle avoidance, and drives its render gait.
  // Returns true while still moving. Shared by followers and scene guides.
  bool StepNpcSteering(ecs::Entity actor, const float goal[3], float pos[3], float rot[4],
                       float speed, float arrive_radius, float stop_radius, f32 dt);
  // Steers every scene guide NPC toward its assigned target (host authoritative;
  // streams to clients via actor sync). A no-op with no guides.
  void UpdateGuides(f32 dt);
  // REC_MQ101_DEMO breadcrumb: once the player exists, walks MQ101 through a
  // curated sequence of gameplay stages. Each frame, if no waypoint is pending,
  // it drops the next one ahead of the player (and recruits nearby NPCs as
  // followers on the first); reaching a waypoint advances the quest to the next
  // stage. The player walks the quest's journal to completion. A grace timeout
  // advances anyway if a waypoint is not reached (stuck terrain / quest teleport),
  // so the guided demo always finishes.
  void Mq101DemoTick(f32 dt);
  // REC_MQ101_SCENE: an NPC-driven escort scene. A guide NPC leads the player
  // along a path while the quest advances stage by stage; the player follows.
  // StartMq101Scene authors and arms it once the player and NPCs exist (returns
  // false to retry); Mq101SceneTick starts it when pending then runs it.
  bool StartMq101Scene();
  void Mq101SceneTick(f32 dt);
  // REC_QUEST_REPORT debug aid: drives the named quest through its stages to
  // completion on the guest thread and prints the journey to stdout.
  void ReportQuestToCompletion(const std::string& edid);
  // REC_DIALOGUE_REPORT debug aid: prints the named quest's dialogue topics,
  // responses, and the fragment each response would run.
  void ReportDialogue(const std::string& edid);
  // Walk-mode activation: finds the form the player faces, shows its HUD prompt,
  // and on `activate_pressed` raises its Papyrus OnActivate (the player as the
  // activator), so scripted refs and NPCs react and can advance quests.
  void UpdateInteraction(bool activate_pressed);
  // Opens a conversation with the NPC `npc` (gathers its condition-gated topics
  // on the guest thread). SelectDialogueOption fires the chosen INFO fragment
  // authoritatively (host runs it; a client asks the server). CloseDialogue ends
  // it. UpdateDialogueInput drives selection/close keys while one is open.
  void OpenDialogue(u64 npc);
  void SelectDialogueOption(int index);
  void CloseDialogue();
  void UpdateDialogueInput(const InputState& input);
  // Runs an INFO record's TIF_ dialogue fragment authoritatively: lazily attaches
  // the script to the INFO handle (seeding its quest property) and calls the
  // begin fragment, which advances the quest. Server / single-player only.
  void RunInfoFragment(u64 info);
  // Builds the activation prompt label, e.g. "Talk to Whiterun Guard" or
  // "Activate Iron Sword", from the reference's base object record.
  std::string ActivationLabel(bethesda::GlobalFormId refr);
  // Enables guest native-call tracing while the trace window is open and
  // snapshots its ring into the overlay (throttled).
  void RefreshNativeTrace(f32 dt);
#if RECREATION_HAS_NET
  bool StartNetworking();
#endif
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
  // A wall hiding a dense grid of small cubes: the gpu hi-z occlusion cull drops
  // the hidden draws (watch "opaque draws: N / M visible" in the debug overlay).
  void CreateOcclusionDemoScene();
  // A dense sphere drawn through the mesh-shader meshlet path, each cluster
  // tinted, with gpu frustum + backface-cone cluster culling.
  void CreateMeshletDemoScene();
  // A row of colored dynamic omni lights over tiles and bumps (forward point
  // lighting + the light-complexity debug view).
  void CreatePointLightDemoScene();

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
  // Builds the player's animated biped (shared rig + a character capsule) at
  // `pos` and makes it actors_[player_actor_]. The capsule rests its feet at
  // pos.y, so callers pass a point on the ground (streamed collision or a demo
  // slab). Does not change walk mode; the caller decides. Used by the actor
  // demo and by REC_PLAYER to drop a walkable avatar into the streamed world.
  bool SpawnPlayerActor(const Vec3& pos);
  // REC_PLAYER (implied by REC_MQ101_DEMO): spawn a walkable player at the
  // loaded cell's start point and enter walk mode, so the streamed world is
  // played on foot with its NPCs instead of flown over. A no-op otherwise.
  void MaybeSpawnWorldPlayer(const Vec3& ground_pos);
  // Loads the shared character rig + meshes into `out` (skeleton, basis, bind
  // pose, body/head/hair parts), uploading each mesh once. Reused by the player
  // actor and as the template every NPC actor is instanced from. Returns false
  // if the core skeleton/body could not load.
  bool LoadActorTemplate(Actor* out);
  // Loads one body-part NIF, uploads it, and appends it to `actor`. Skinned
  // parts bind to the skeleton by name; a part with no skin falls back to a
  // rigid mesh riding `attach_bone` (>= 0), used for the head and hair.
  bool LoadActorPart(const std::string& path, Actor& actor, i32 attach_bone = -1);
  // Gives every placed NPC entity a skinned actor (instanced from the shared
  // template) and drops actors whose NPC entity has streamed out. Non-headless.
  void SyncNpcActors();
  // Maintains a kinematic collision capsule for each NPC and each other player,
  // tracking its entity transform, so the local player's character collides with
  // them (solid bodies; the shove pushes them). Drops bodies whose entity left.
  void SyncSolidBodies();
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
  // Teleports the player actor (capsule + ECS transform), the target of a quest
  // MoveTo on the player. Coordinates are passed through as given.
  void TeleportPlayer(f32 x, f32 y, f32 z);
  // Raises OnActivate(player) on a reference's script instance. The authoritative
  // entry point: a host calls it directly, the server calls it for a client's
  // activation request, and a client routes activations to the server instead.
  void RaiseActivate(u64 handle);
  // Drains quest world commands into QuestWorld on the main thread, and (when
  // hosting) replicates the batch to clients.
  void ApplyQuestWorld();
  // Server-side NPC simulation (host / single-player): players shove nearby NPCs
  // out of the way. The moved transforms then stream to clients via actor sync.
  void ServerSimulateActors(f32 dt);
  // Advances every actor's gait and recomputes its model-space bone matrices.
  void UpdateActors(f32 dt);
  void UpdateOneActor(Actor& actor, f32 dt);
  // Appends each actor's skinned draws + bone palettes to the frame view.
  void EmitActorDraws(render::FrameView& view);
  void EmitOneActor(Actor& actor, render::FrameView& view);

  bool particles_enabled_ = false;
  Vec3 particle_emitter_{0, 0, 0};
  u32 gpu_particle_count_ = 0;  // > 0 selects the gpu-simulated fountain
  Vec3 gpu_particle_emitter_{0, 0, 0};
  bool fur_ball_ = false;
  Vec3 fur_position_{0, 0, 0};
  base::Vector<render::WboitInstance> oit_instances_;
  base::Vector<DemoParticle> demo_particles_;
  base::Vector<render::GaussianInstance> demo_gaussians_;
  base::Vector<render::PointLight> demo_lights_;
  u32 particle_seed_ = 0x9e3779b9u;
  f32 particle_spawn_accum_ = 0;

  EngineConfig config_;
  bethesda::Game game_ = bethesda::Game::kUnknown;

  std::unique_ptr<Window> window_;
  std::unique_ptr<JobSystem> jobs_;
  FrameTimer timer_;

  ecs::World world_;
  ecs::Scheduler scheduler_;
  // Quest-driven world effects: the bindings push commands onto the queue (guest
  // thread); the main thread drains them into QuestWorld, which spawns/moves ECS
  // entities and records per-quest provenance so a quest can be rolled back.
  world::WorldCommandQueue quest_world_queue_;
  world::QuestWorld quest_world_{world_};
  RuntimeWorldSink runtime_world_sink_{&quest_world_queue_};

  asset::Vfs vfs_;
  std::unique_ptr<asset::AssetDatabase> assets_;
  bethesda::RecordStore records_;
  // Localized FULL/log/objective text for records (quest names, journal text).
  bethesda::StringTable strings_;
  // DIAL topics indexed by quest, for NPC dialogue.
  dialogue::DialogueDb dialogue_;
  DialogueSession dialogue_session_;
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
  // HUD quest tracker: the quest currently shown and the revision last shown,
  // so a stage/objective change raises the "quest updated" banner once.
  u64 hud_tracked_quest_ = 0;
  u32 hud_tracked_revision_ = 0;
  // Objective waypoints (host authoritative). Armed against the tracked quest's
  // current displayed objective; reaching one advances the quest's stage.
  base::Vector<QuestMarker> quest_markers_;
  // Objective marker replication. Host: the last marker state sent to clients,
  // so it only resends on change. Client: the host's marker, driving the local
  // HUD pip from this peer's own camera.
  bool sent_marker_active_ = false;
  Vec3 sent_marker_pos_{};
  u64 sent_marker_quest_ = 0;
  bool remote_marker_active_ = false;
  Vec3 remote_marker_pos_{};
  // NPCs steered to follow the player, keyed by form handle -> formation slot.
  base::UnorderedMap<u64, i32> followers_;
  // Scene guide NPCs steered toward a fixed world target, keyed by form handle.
  base::UnorderedMap<u64, Vec3> guides_;
  // REC_MQ101_DEMO breadcrumb state: the quest, the ordered gameplay stages to
  // walk it through, and the index of the next waypoint. Active while pending.
  bool mq101_demo_pending_ = false;
  u64 mq101_demo_quest_ = 0;
  base::Vector<i32> mq101_demo_stages_;
  size_t mq101_demo_next_ = 0;
  f32 mq101_demo_wait_ = 0;  // seconds the current waypoint has gone unreached
  // REC_MQ101_SCENE escort: the authored scene, its runner, the engine sink, and
  // the pending/active flags.
  quest::Scene mq101_scene_;
  quest::SceneRunner scene_runner_;
  Mq101Sink scene_sink_;
  bool mq101_scene_pending_ = false;
  bool mq101_scene_active_ = false;
  i32 mq101_scene_stuck_ticks_ = 0;  // ticks the current beat has not progressed
  // Activation: the form the player is looking at in walk mode (0 = none) and
  // the cached HUD label, recomputed only when the target changes.
  u64 activate_target_ = 0;
  std::string activate_label_;
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
  // Placed-NPC actors, keyed by ECS entity (generation<<32 | index). Separate
  // from actors_ so streaming can add/remove them without disturbing the player
  // index. Each is instanced from npc_template_ (own pose, shared GPU meshes).
  std::optional<Actor> npc_template_;
  bool npc_template_failed_ = false;
  base::UnorderedMap<u64, Actor> npc_actors_;
  base::Vector<u64> scratch_dead_actors_;
  // Kinematic collision capsules for NPCs + other players, keyed like npc_actors_.
  base::UnorderedMap<u64, physics::BodyId> solid_bodies_;

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
#if RECREATION_HAS_NET
  std::unique_ptr<net::Session> session_;
  // Typed views into session_, null unless that role is active.
  net::ServerSession* server_session_ = nullptr;
  net::ClientSession* client_session_ = nullptr;
#endif
  f32 demo_input_time_ = 0;

  std::atomic<bool> quit_ = false;
  bool shut_down_ = false;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_ENGINE_H_
