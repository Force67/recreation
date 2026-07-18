#ifndef RECREATION_RUNTIME_ENGINE_H_
#define RECREATION_RUNTIME_ENGINE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "actor_system.h"
#include "app/application.h"
#include "app/host.h"
#include "audio/ambient.h"
#include "bethesda/planet.h"
#include "chargen.h"
#include "content_domain.h"
#include "core/input_bindings.h"
#include "core/window.h"
#include "core/world_clock.h"
#include "demo_scenes.h"
#include "editor.h"
#include "engine_context.h"
#include "interaction_system.h"
#include "item_bridge.h"
#include "npc_director.h"
#include "platform_hud.h"
#include "quest_director.h"
#include "script/host/managed_host.h"
#include "showcase_camera.h"
#include "player_controller.h"
#include "trailer.h"
#include "weather/director.h"
#include "weather/weather.h"
#include "world/combat.h"
#include "world/planet_tile.h"

#if RECREATION_HAS_NET
#include "modstream/content_store.h"
#include "modstream/mod_catalog.h"
#endif

namespace rx {

// WorldEffectSink implementation: the Skyrim bindings call this on the guest
// thread; it allocates handles and marshals each mutation into the thread-safe
// WorldCommandQueue, which the main thread drains into QuestWorld. Kept tiny and
// header-only so the script module need not know about the ECS.
class RuntimeWorldSink : public script::WorldEffectSink {
 public:
  RuntimeWorldSink(world::WorldCommandQueue* queue, world::CombatEventQueue* combat)
      : queue_(queue), combat_(combat) {}

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
  void MovePlayer(u64 quest, u64 dest_ref, f32 x, f32 y, f32 z) override {
    Emit(world::WorldOp::kMovePlayer, quest, dest_ref, x, y, z);
  }
  void SetEnabled(u64 quest, u64 handle, bool enabled) override {
    world::WorldCommand c;
    c.op = world::WorldOp::kSetEnabled;
    c.quest = quest;
    c.handle = handle;
    c.enabled = enabled;
    queue_->Push(c);
  }
  void SetLocked(u64 quest, u64 handle, bool locked) override {
    EmitState(world::WorldOp::kSetLocked, quest, handle, locked);
  }
  void SetOpen(u64 quest, u64 handle, bool open) override {
    EmitState(world::WorldOp::kSetOpen, quest, handle, open);
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
  void StartCombat(u64 /*quest*/, u64 attacker, u64 target) override {
    combat_->Push({world::CombatOp::kEngage, attacker, target});
  }
  void StopCombat(u64 /*quest*/, u64 attacker) override {
    combat_->Push({world::CombatOp::kDisengage, attacker, 0});
  }
  void ActorDied(u64 /*quest*/, u64 actor) override {
    combat_->Push({world::CombatOp::kDied, actor, 0});
  }
  void ActorResurrected(u64 /*quest*/, u64 actor) override {
    combat_->Push({world::CombatOp::kResurrected, actor, 0});
  }
  void ActorFollow(u64 /*quest*/, u64 actor, bool follow) override {
    combat_->Push({follow ? world::CombatOp::kFollow : world::CombatOp::kUnfollow, actor, 0});
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

  void EmitState(world::WorldOp op, u64 quest, u64 handle, bool value) {
    world::WorldCommand c;
    c.op = op;
    c.quest = quest;
    c.handle = handle;
    c.enabled = value;
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
  world::CombatEventQueue* combat_;
  std::atomic<u32> next_handle_{1};
};

// The game: recreation's app::Application. Owns the gameplay layer (actors,
// interaction, quest, npc, demos, weather, networking, data loading, the camera
// and the UI) and drives it from the app::Host callbacks; the generic
// subsystems and the fixed-step loop live in app::Host. The gameplay subsystems
// own their own state and are driven from here through the EngineContext, which
// is filled from the host's Services.
class Engine : public app::Application {
 public:
  explicit Engine(const EngineConfig& config) : config_(config) {}
  ~Engine() override;

  // app::Application: the host brings the subsystems up, then hands them over in
  // `services`. Loads content, registers ECS systems and spawns the world.
  bool OnInitialize(app::Services& services) override;
  // Frame-cadence game simulation (scripting, quests, npc/combat, networking);
  // runs in both windowed and headless (dedicated-server) modes.
  void OnSimulate(f32 frame_delta) override;
  // Windowed-only per-frame policy: weather/sky, camera, menus, UI begin.
  void OnUpdate(f32 frame_delta) override;
  // Windowed-only: builds this frame's FrameView (camera, gathered draws,
  // actors, lights, decals, HUD).
  void OnBuildView(f32 frame_delta, render::FrameView& view) override;
  // Windowed-only: the capture/quit hooks after the frame submitted.
  void OnFrameEnd() override;
  // The renderer is idle but alive: drop the game's GPU-dependent resources.
  void OnShutdown() override;

  // Safe to call from a signal handler; the host's Run() returns after the
  // current frame. Forwards to the host once OnInitialize has run.
  void RequestQuit() {
    if (host_) host_->RequestQuit();
  }

  // Global debug toggles set by Debug.* console natives (tgm/tcl/tai/tm, foot IK).
  // Tracked here so the state persists and any system can honour it.
  struct DebugFlags {
    bool god_mode = false;
    bool ai_disabled = false;
    bool collisions_disabled = false;
    bool menus_hidden = false;
    bool foot_ik = true;
  };
  const DebugFlags& debug_flags() const { return debug_flags_; }

#if RECREATION_HAS_NET
  // Requests a live reload of the streamed mods (rebuild the catalog, re-offer to
  // joining clients, re-mount on the host). Safe from a signal handler; applied on
  // the main thread next frame. A no-op when not hosting a mods directory.
  void RequestModReload() { mod_reload_requested_.store(true, std::memory_order_relaxed); }
#endif

 private:
  // The bring-up steps are free functions over the engine (declared just below
  // the class, defined in content_load.cc / networking.cc / managed_scripting.cc
  // / main_menu.cc); they reach the engine's internals as friends.
  friend bool LoadGameData(Engine&);
  friend void MountArchives(Engine&);
  friend bool LoadInterior(Engine&);
  friend bool LoadPlanetTile(Engine&, const std::string&);
  friend void LoadExtraDomains(Engine&);
  friend void SetupExtraStreamers(Engine&);
  friend void BootManagedScripting(Engine&);
  friend void ResolveUniverses(Engine&);
  friend void SetupMainMenu(Engine&);
  friend void EnterUniverse(Engine&, int, bool, bool, const std::string&);
  friend void SetupFirstRun(Engine&);
  friend void LoadSetupConfig(Engine&);
#if RECREATION_HAS_NET
  friend bool StartNetworking(Engine&);
  friend void ReloadMods(Engine&);
  friend void EngineRpcEmitImpl(Engine&, std::int32_t, std::uint64_t, const char*,
                                const script::host::ApiValue*, std::int32_t);
  friend void EngineRpcSubscribeImpl(Engine&, const char*);
  friend void RegisterManagedRpcForwarding(Engine&);
#endif

  // NEXUS main menu, per frame: UpdateMainMenu drives nav + dispatch (it enters a
  // universe through the free EnterUniverse); RefreshMenuData feeds it the live
  // player / network / mods data.
  void UpdateMainMenu(f32 dt);
  void RefreshMenuData();
  // First-run setup wizard, per frame: drives Next/Back and dispatches the
  // wizard's requests (open a folder picker, launch into the main menu, cancel).
  // Active only on a fresh install, before SetupMainMenu takes over.
  void UpdateFirstRun(f32 dt);
  // Paints the three universe panes (Skyrim / Fallout 4 / Starfield) as original,
  // per-pixel procedural concept art — atmospheric sky, silhouettes, grain — and
  // uploads them as the menu's pane backdrop textures. No external image assets:
  // the front screen is self-contained and non-infringing.
  void GenerateMenuBackdrops();
  // Per-frame: a few seconds after entering a universe, grabs one clean frame of
  // its world (HUD/overlays hidden) into the backdrop cache for next time.
  void TickMenuCapture();
  bool LoadGltfScene();

  void ThrowPhysicsCube();
  void UpdateCamera(f32 frame_delta);
  // Camera record/replay (deterministic playback for benchmarks and capture).
  // RX_ORBIT turntables the camera, RX_RECORD=<path> writes the path each
  // frame, RX_REPLAY=<path> drives the camera from a recorded path.
  void DriveCamera(f32 dt);
  void LookCameraAt(const Vec3& eye, const Vec3& center);
  // Builds the cinematic showcase path (RX_SHOWCASE): a smooth drone pass over
  // each loaded worldspace in turn, from the region centers gathered at load.
  void BuildShowcase();
  // Builds the trailer timeline (RX_TRAILER) over the showcase path: a location
  // title per region, plus the weather + render-mode cycles.
  void BuildTrailer();
  // Maps a trailer render mode onto the renderer's feature flags (raster vs
  // ray-traced vs the reference path tracer).
  void ApplyTrailerRenderMode(TrailerRenderMode mode);
  // Multi-game trailer: collapse every region onto one shared center and stream
  // them around the camera one at a time (so the maps do not all sit resident in
  // the shared scene). Only does anything when extra games are loaded.
  void SetupTrailerStreaming();
  // Switches which game is resident: unloads the outgoing one's cells, leaving
  // the incoming to stream in (the fade-cut hides the swap).
  void SwitchTrailerDomain(int region_index);
  // The streamer owning showcase region `index` (the primary streamer_ for the
  // first region / a null region streamer); null if out of range.
  world::CellStreamer* TrailerStreamer(int region_index);
  // True when the active trailer domain has streamed in (or there is none), so
  // the camera can stop holding on the loading screen and reveal it.
  bool TrailerActiveLoaded();
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

  // The three universes the NEXUS main menu offers, in column order (Skyrim,
  // Fallout 4, Starfield); resolved at menu setup from --data-dir/--add-game,
  // env overrides (RX_SKYRIM_DATA/RX_FALLOUT4_DATA/RX_STARFIELD_DATA) or a
  // scan of the Steam libraries. main_menu_active_ is true while the menu owns
  // the screen, before a universe has been entered.
  struct MenuUniverse {
    bethesda::Game game = bethesda::Game::kUnknown;
    std::string name;
    std::string data_dir;
    std::string plugins_txt;
    bool available = false;
  };
  std::array<MenuUniverse, 3> menu_universes_;
  bool main_menu_active_ = false;
  // First-run out-of-box wizard: owns the screen on a fresh install until the
  // player finishes setup, at which point it hands off to the main menu. The
  // mods directory the wizard collects is held here until it is persisted.
  bool first_run_active_ = false;
  std::string first_run_mods_dir_;
  // Deferred capture of the entered world into the backdrop cache: counts down
  // after EnterUniverse, hiding the HUD for the grab frame so the cached scene
  // is clean. Idle at 0.
  int menu_capture_countdown_ = 0;
  std::string menu_capture_path_;

  // The app::Host owns the window/jobs/frame-timer/clock and drives the loop;
  // these are non-owning views cached from Services at OnInitialize.
  app::Host* host_ = nullptr;
  Window* window_ = nullptr;  // null when headless
  // The in-world clock driving the day/night cycle, owned and advanced by the
  // host; the Papyrus time natives read it through the bindings, and the render
  // loop derives the sun/sky from it. drive_sun_from_clock_ is false when
  // RX_SUN_DIR pins a fixed sun (headless lighting tests), leaving the sun
  // static. last_sky_hour_ throttles the sun update so the IBL environment is
  // not rebuilt every frame for sub-degree motion.
  WorldClock* clock_ = nullptr;
  bool drive_sun_from_clock_ = true;
  f32 last_sky_hour_ = -1000.0f;
  // Weather, parsed from the game's WTHR/CLMT/REGN and driven off the world
  // clock through one system: selection, region overrides, cross-fades,
  // lightning strikes, thunder audio and the wetness/snow integrators all live
  // in the director; the frame loop gathers a Tick and consumes the blended
  // state for sun tinting. last_weather_* let the throttled sun update re-fire
  // when the weather light changes.
  weather::Director director_;
  f32 last_weather_scale_ = 1.0f;
  Vec3 last_weather_tint_{1, 1, 1};

  // ECS world + scheduler, owned by the host; non-owning views cached here.
  ecs::World* world_ = nullptr;
  ecs::Scheduler* scheduler_ = nullptr;
  // Quest-driven world effects: the bindings push commands onto the queue (guest
  // thread); the main thread drains them into QuestWorld, which spawns/moves ECS
  // entities and records per-quest provenance so a quest can be rolled back.
  world::WorldCommandQueue quest_world_queue_;
  // Built in OnInitialize once the host's ECS world is available (it holds a
  // World&, so it cannot be constructed before the world exists).
  std::unique_ptr<world::QuestWorld> quest_world_;
  // Guest -> main combat enrollment (StartCombat/StopCombat/death), drained each
  // frame into the npc director's combat driver.
  world::CombatEventQueue combat_event_queue_;
  RuntimeWorldSink runtime_world_sink_{&quest_world_queue_, &combat_event_queue_};

  // The Vfs + audio system are owned by the host; non-owning views cached here.
  // Audio reads sound bytes lazily through the Vfs; the sound catalog (SOUN/SNDR
  // -> file) and region ambience (REGN -> sounds) are built once game data loads;
  // the director cross-fades the ambient bed as the player's region changes.
  asset::Vfs* vfs_ = nullptr;
  audio::AudioSystem* audio_ = nullptr;
  audio::SoundCatalog sound_catalog_;
  audio::RegionAmbience region_ambience_;
  audio::AmbientDirector ambient_director_;
  std::unique_ptr<asset::AssetDatabase> assets_;
  bethesda::RecordStore records_;
  // Localized FULL/log/objective text for records (quest names, journal text).
  bethesda::StringTable strings_;
  // DIAL topics indexed by quest, for NPC dialogue.
  dialogue::DialogueDb dialogue_;
  std::unique_ptr<world::CellStreamer> streamer_;
  // Procedural Starfield planet tile (RX_STARFIELD_PLANET): the generator plus
  // the resolved surface it reads from, kept alive for the session (the ground
  // query and any later regeneration reference them). Null in every other mode.
  std::unique_ptr<bethesda::PlanetSurface> planet_surface_;
  std::unique_ptr<world::PlanetTile> planet_tile_;
  // One streamer per --add-game that renders, each streaming its own worldspace
  // into the shared scene at a fixed offset (so Fallout 4's Commonwealth sits
  // beside Skyrim's Tamriel instead of overlapping it). Parallel to the matching
  // entries in extra_domains_; cleared before them in Shutdown.
  base::Vector<std::unique_ptr<world::CellStreamer>> extra_streamers_;
  // Declared before scripts_ so the guest thread (which calls into the bindings)
  // is joined in ScriptSystem's destructor before the bindings are torn down.
  std::unique_ptr<rx::script::skyrim::RecordBackedSkyrimBindings> script_bindings_;
  std::unique_ptr<rx::script::ScriptSystem> scripts_;
  // Additional games loaded as live secondary content domains (Fallout 4 next to
  // Skyrim, say). Each owns its data and an isolated Papyrus microvm, ticked
  // every frame. Declared after scripts_ so the primary guest is unaffected by
  // their teardown; cleared explicitly in Shutdown before the managed host.
  base::Vector<std::unique_ptr<ContentDomain>> extra_domains_;
  // The managed (C#) scripting world, where user mods and Skyrim soft logic run.
  // Declared after scripts_ so it tears down before the guest thread it drives.
  // Null when .NET or the assembly is unavailable, leaving the engine unaffected.
  std::unique_ptr<rx::script::host::ManagedHost> managed_;
  // Reused buffer for the per-frame position snapshot handed to the bindings'
  // proximity query. Main-thread only.
  std::vector<std::pair<u64, std::array<f32, 3>>> position_snapshot_;
  // Previous frame's positions, to derive each ref's speed for Actor.IsRunning.
  std::unordered_map<u64, std::array<f32, 3>> prev_positions_;

  render::Renderer* renderer_ = nullptr;  // owned by the host
  FlyCamera camera_;
  // Device-agnostic input: bindings + the per-frame resolved action snapshot.
  // Raw window_->input() / gamepad() stay available for text fields and the C#
  // key bridge; gameplay reads actions_ instead of hardcoded keys.
  // Owned by the host: the input bindings and the per-frame resolved action
  // snapshot the host fills each pump. Non-owning views cached here.
  InputMap* input_map_ = nullptr;
  const ActionState* actions_ = nullptr;
  // Controls config persistence + in-game rebinding (controls_settings.cc).
  void LoadControls();    // read controls.ini into input_map_, then ApplyControls
  void SaveControls();    // write input_map_ back to controls.ini
  void ApplyControls();   // push sensitivity/invert to the camera, LED to the pad
  void UpdateSettings();  // drive the settings panel: rebind capture + sliders
  std::string controls_path_;
  int capturing_row_ = -1;           // settings: row awaiting an input (-1 = idle)
  bool capture_prev_mouse_[3] = {};  // mouse-button edge tracking during capture
  bool weapon_trigger_ = false;      // DualSense adaptive-trigger weapon state

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

  // Cinematic showcase (RX_SHOWCASE): a smooth drone flythrough over every
  // loaded worldspace in one take, doubling as a deterministic benchmark and a
  // source of regression frames (RX_SHOWCASE_SHOTS=<dir>). The region centers
  // are gathered at ground level as each worldspace is placed.
  struct ShowcaseRegion {
    Vec3 center{};     // ground-level center of the worldspace to fly over
    std::string name;  // game/profile name, used in capture filenames
    // The streamer that owns this region's content (null = the primary streamer_).
    // The trailer uses it to keep only the active game resident.
    world::CellStreamer* streamer = nullptr;
  };
  base::Vector<ShowcaseRegion> showcase_regions_;
  ShowcaseCamera showcase_;
  bool cam_showcase_ = false;
  bool showcase_done_ = false;
  bool showcase_quit_ = false;  // RX_SHOWCASE_QUIT: exit when the pass ends
  std::string showcase_shot_dir_;
  f32 showcase_dt_min_ = 1e9f;
  f32 showcase_dt_max_ = 0;
  f32 showcase_bench_time_ = 0;  // summed dt of benchmarked frames (excludes load hitches)
  u32 showcase_frames_ = 0;
  // Flythrough time the camera starts gliding toward each region, parallel to
  // showcase_regions_; used to fade the trailer location titles in on cue.
  base::Vector<f32> showcase_region_start_;

  // Trailer overlay (RX_TRAILER): layered over the showcase, it cycles weather
  // and the render mode and titles each map. current_trailer_overlay_ is the
  // chrome the debug overlay draws; the render mode is only re-applied on change.
  TrailerDirector trailer_;
  bool cam_trailer_ = false;
  TrailerOverlay current_trailer_overlay_;
  TrailerRenderMode applied_trailer_mode_ = TrailerRenderMode::kRayTracing;
  bool trailer_mode_applied_ = false;
  // Multi-game trailer: when set, the showcase flies over one shared center and
  // only trailer_active_domain_ (a showcase region index) streams at a time.
  bool trailer_sequential_ = false;
  int trailer_active_domain_ = 0;
  // Loading hold: while true the trailer clock is frozen and the screen shows a
  // loading card, so a freshly cut-to game streams in before the camera reveals
  // it. trailer_load_elapsed_ is wall-clock since the hold began (a safety cap).
  bool trailer_loading_ = false;
  f32 trailer_load_elapsed_ = 0.0f;

  DebugUi debug_ui_;
  GameUi game_ui_;
  // Debug.Notification messages awaiting display, pushed from the guest thread and
  // drained to the HUD toast on the main loop.
  std::mutex notification_mutex_;
  std::vector<std::string> pending_notifications_;
  // Debug.* engine commands (quit, screenshot, toggles) pushed from the guest
  // thread and applied on the main loop via ApplyDebugCommand.
  std::mutex debug_cmd_mutex_;
  std::vector<std::pair<std::string, std::string>> pending_debug_cmds_;
  DebugFlags debug_flags_;
  int screenshot_index_ = 0;
  void ApplyDebugCommand(const std::string& verb, const std::string& arg);
  // Multiplayer platform HUD/Net calls (chat, notifications, prompts, scoreboard,
  // blips) pushed from the guest thread, drained onto the HUD on the main loop.
  PlatformHud platform_hud_;
  // Accumulated chat lines shown in the chat box (bounded tail of the channel).
  std::vector<std::string> platform_chat_display_;
  // Networked entities a mod spawned (NetEntity id -> the ECS entity placed for it),
  // so a later move/delete finds the same object.
  base::UnorderedMap<int, ecs::Entity> net_entities_;
  // A placeable base form (a static with a model) used as the placeholder visual
  // for spawned net entities until per-model meshes are wired. Resolved once.
  bethesda::GlobalFormId net_entity_base_{};
  bethesda::GlobalFormId net_entity_base_fallback_{};  // any static, if no nice prop
  bool net_entity_base_ready_ = false;
  // Resolved NetEntity model (editor id) -> base form, so a mod spawns a specific
  // object by name. Cached because the lookup scans the record store.
  std::unordered_map<std::string, bethesda::GlobalFormId> net_model_cache_;
  physics::PhysicsWorld* physics_ = nullptr;  // owned by the host
  // Dynamic bodies mirrored into ECS transforms after each step.
  base::Vector<PhysicsEntity> physics_entities_;
  asset::AssetId physics_cube_mesh_;

  f32 cam_pitch_ = -0.15f;
  f32 auto_attack_timer_ = 0;  // RX_AUTO_ATTACK swing cadence (playthrough verification)
  bool war_map_open_ = false;  // Civil War war-map overlay (toggled with M)
  // Last frame's world matrices keyed by entity, for motion vectors.
  base::UnorderedMap<u64, Mat4> prev_transforms_;
#if RECREATION_HAS_NET
  std::unique_ptr<net::Session> session_;
  // Typed views into session_, null unless that role is active.
  net::GameServerSession* server_session_ = nullptr;
  net::GameClientSession* client_session_ = nullptr;
  // Asset streaming: the host's catalogued mods directory, and the client's
  // content cache. The session holds pointers into these, so they outlive it.
  std::unique_ptr<modstream::ModCatalog> mod_catalog_;
  std::unique_ptr<modstream::ContentStore> content_store_;
  // Scripting RPC names the managed world subscribed to (before the session
  // exists, since managed boots first). StartNetworking forwards each of these
  // from the session into managed code.
  std::vector<std::string> managed_rpc_names_;
  // Set from a signal handler to ask for a live mod reload; drained on the main
  // thread at the top of the frame, where the Vfs is not being read.
  std::atomic<bool> mod_reload_requested_{false};
  // 3D overlay of the session's streaming bubbles (RX_NET_BUBBLES=0 hides it).
  // Built lazily on the first frame that has bubbles to draw.
  std::unique_ptr<net::BubbleVisualizer> bubble_viz_;
#endif

  // REC_NAV_DEBUG overlay storage: rebuilt each frame, spanned into the view.
  base::Vector<render::DebugLine> nav_debug_lines_;

  // Shared service bundle handed to the subsystems, plus the subsystems
  // themselves (built in Initialize once the context is populated).
  EngineContext ctx_;
  std::unique_ptr<ActorSystem> actors_;
  // Skyrim player locomotion + FP/TP camera (rx character + camera-rig pipeline).
  // Built lazily on the first walk-mode frame once the player actor exists.
  std::unique_ptr<PlayerController> player_controller_;
  std::unique_ptr<InteractionSystem> interaction_;
  std::unique_ptr<ItemBridge> items_;  // item pickup/drop/persistence
  std::unique_ptr<NpcDirector> npc_;
  std::unique_ptr<QuestDirector> quest_;
  std::unique_ptr<DemoScenes> demos_;
  // Live map editor (windowed client only); F4 toggles it. Null in headless.
  std::unique_ptr<MapEditor> editor_;
  // Character-creation screen (RX_CHARGEN boot mode). Null in headless.
  std::unique_ptr<CharGen> chargen_;
};

// Engine bring-up steps, written as free functions over the engine (each a
// friend of Engine; see content_load.cc / networking.cc / managed_scripting.cc /
// main_menu.cc). LoadGameData mounts archives, loads the record/string/dialogue
// data, stands up the Papyrus guest + bindings and the cell streamer(s);
// MountArchives, LoadInterior, LoadExtraDomains and SetupExtraStreamers are its
// steps.
bool LoadGameData(Engine& engine);
void MountArchives(Engine& engine);
bool LoadInterior(Engine& engine);
// Boots a synthesized procedural Starfield planet tile (RX_STARFIELD_PLANET).
bool LoadPlanetTile(Engine& engine, const std::string& biom_name);
void LoadExtraDomains(Engine& engine);
void SetupExtraStreamers(Engine& engine);
// Boots the managed (C#) scripting world over the live guest, if a .NET runtime
// and the Recreation.Scripting assembly are available (RECREATION_SCRIPTING_DIR).
// A no-op on a replica client, where scripts run server-authoritative.
void BootManagedScripting(Engine& engine);
// NEXUS main menu. ResolveUniverses fills menu_universes_ (the three games' data
// dirs from args / env / a Steam scan); SetupMainMenu opens the menu without
// loading a game; EnterUniverse loads the chosen game on demand so its C#
// gameplay module boots (the module gates on being the primary domain).
void ResolveUniverses(Engine& engine);
void SetupMainMenu(Engine& engine);
void EnterUniverse(Engine& engine, int idx, bool multiplayer, bool host,
                   const std::string& join_address);
// First-run out-of-box setup. LoadSetupConfig pulls any persisted game paths /
// mods dir into the EngineConfig before universes are resolved. FirstRunComplete
// reports whether setup has already been finished (a marker file exists).
// SetupFirstRun opens the wizard with the games pre-resolved, so found ones show
// as located. On launch the wizard persists its choices and hands off to
// SetupMainMenu.
void LoadSetupConfig(Engine& engine);
bool FirstRunComplete();
void SetupFirstRun(Engine& engine);
#if RECREATION_HAS_NET
// Opens the authoritative server or replica client session and wires the
// replication sinks between the net layer and the script/quest systems.
bool StartNetworking(Engine& engine);
// Builds the multiplayer RPC surface handed to the managed world (before Boot,
// before the session exists): emit routes to the live session, on records a
// subscription StartNetworking later forwards.
script::host::RpcBridge MakeManagedRpcBridge(Engine& engine);
// Registers a forwarding handler on the live session's RPC registry for every
// name the managed world subscribed to, so inbound calls reach C#. Called once
// the session is up.
void RegisterManagedRpcForwarding(Engine& engine);
// Live-reloads the streamed mods: rebuilds the catalog from the mods directory,
// re-offers it to joining clients, and re-mounts it on the host Vfs. Keeps the
// current set if the rebuild fails (a misconfigured edit must not break the live
// server). Main thread only.
void ReloadMods(Engine& engine);
#endif

}  // namespace rx

#endif  // RECREATION_RUNTIME_ENGINE_H_
