#ifndef RECREATION_RUNTIME_ENGINE_H_
#define RECREATION_RUNTIME_ENGINE_H_

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/frame_timer.h"
#include "core/job_system.h"
#include "core/window.h"

#include "actor_system.h"
#include "demo_scenes.h"
#include "engine_context.h"
#include "interaction_system.h"
#include "npc_director.h"
#include "quest_director.h"

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
    c.pos = ToEngine(x, y, z);
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
    c.pos = ToEngine(x, y, z);
    queue_->Push(c);
  }

  // Bethesda game space (Z-up, ~70 units/m) to engine space (Y-up, metres),
  // axes (x, z, -y). The bindings speak game units (Papyrus reads them back), so
  // every position they hand the ECS is converted here at the one crossing
  // point; QuestWorld then treats command positions as engine space.
  static std::array<f32, 3> ToEngine(f32 x, f32 y, f32 z) {
    constexpr f32 s = 0.01428f;
    return {x * s, z * s, -y * s};
  }

  world::WorldCommandQueue* queue_;
  std::atomic<u32> next_handle_{1};
};

// Top-level orchestrator. Owns the shared services, the main loop, networking,
// data loading and the camera; the gameplay subsystems (actors, interaction,
// quest, npc, demos) own their own state and are driven from here through the
// EngineContext.
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
  bool LoadGameData();
  bool LoadInterior();
  void MountArchives();
  bool LoadGltfScene();
#if RECREATION_HAS_NET
  bool StartNetworking();
#endif
  // Resolves the configured quality tier from the gpu (or a forced preset) and
  // applies it to the renderer's live settings.
  void ApplyRenderPreset();

  void ThrowPhysicsCube();
  void UpdateCamera(f32 frame_delta);
  // Camera record/replay (deterministic playback for benchmarks and capture).
  // REC_ORBIT turntables the camera, REC_RECORD=<path> writes the path each
  // frame, REC_REPLAY=<path> drives the camera from a recorded path.
  void DriveCamera(f32 dt);
  void LookCameraAt(const Vec3& eye, const Vec3& center);
  // Walk mode step: input -> character move (via the actor system) -> follow
  // camera. The player capsule lives in the actor system; this computes intent.
  void WalkUpdate(f32 dt, bool allow_input);
  // Drains quest world commands into QuestWorld on the main thread, and (when
  // hosting) replicates the batch to clients.
  void ApplyQuestWorld();
  // Server-side NPC simulation (host / single-player): players shove nearby NPCs
  // out of the way. The moved transforms then stream to clients via actor sync.
  void ServerSimulateActors(f32 dt);

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
  GameUi game_ui_;
  physics::PhysicsWorld physics_;
  // Dynamic bodies mirrored into ECS transforms after each step.
  base::Vector<PhysicsEntity> physics_entities_;
  asset::AssetId physics_cube_mesh_;

  f32 cam_pitch_ = -0.15f;
  // Last frame's world matrices keyed by entity, for motion vectors.
  base::UnorderedMap<u64, Mat4> prev_transforms_;
#if RECREATION_HAS_NET
  std::unique_ptr<net::Session> session_;
  // Typed views into session_, null unless that role is active.
  net::ServerSession* server_session_ = nullptr;
  net::ClientSession* client_session_ = nullptr;
#endif

  // Shared service bundle handed to the subsystems, plus the subsystems
  // themselves (built in Initialize once the context is populated).
  EngineContext ctx_;
  std::unique_ptr<ActorSystem> actors_;
  std::unique_ptr<InteractionSystem> interaction_;
  std::unique_ptr<NpcDirector> npc_;
  std::unique_ptr<QuestDirector> quest_;
  std::unique_ptr<DemoScenes> demos_;

  std::atomic<bool> quit_ = false;
  bool shut_down_ = false;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_ENGINE_H_
