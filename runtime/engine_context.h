#ifndef RECREATION_RUNTIME_ENGINE_CONTEXT_H_
#define RECREATION_RUNTIME_ENGINE_CONTEXT_H_

#include <string>

#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "audio/audio_system.h"
#include "bethesda/game_profile.h"
#include "bethesda/record.h"
#include "bethesda/strings.h"
#include "core/math.h"
#include "debug_ui.h"
#include "dialogue/dialogue.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "fly_camera.h"
#include "game_ui.h"
#include "physics/physics_world.h"
#include "render/core/presets.h"
#include "render/core/renderer.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/host/managed_host.h"
#include "script/script_system.h"
#include "world/cell_streaming.h"
#include "world/quest_world.h"

#if RECREATION_HAS_NET
#include "gamenet/session.h"
#include "net/bubble_debug.h"
#endif

namespace rx {

class ItemBridge;
struct ActionState;  // core/input_actions.h; resolved device-agnostic input

// An additional game loaded as a live secondary content domain alongside the
// primary (rendered) game. Each runs its own isolated Papyrus microvm, so
// Skyrim and Fallout 4 content stay live in one process at the same time.
struct ExtraDomainConfig {
  bethesda::Game game = bethesda::Game::kUnknown;  // kUnknown = autodetect
  std::string data_dir;
  std::string plugins_txt;
};

struct EngineConfig {
  std::string data_dir;
  std::string plugins_txt;
  base::Vector<ExtraDomainConfig> extra_domains;
  std::string gltf_path;  // standalone gltf/glb scene (e.g. sponza)
  std::string demo_scene;  // "water" = empty map with a water sheet
  // Open the NEXUS main menu instead of loading content directly; the player
  // picks a universe there and the engine loads it on demand (EnterUniverse).
  // Defaults on for a bare windowed launch with no content source.
  bool main_menu = false;
  // Spawn a walkable player and enter walk mode after the world loads (set when
  // entering a universe from the main menu, alongside the RX_PLAYER env gate).
  bool spawn_player = false;
  bethesda::Game game = bethesda::Game::kUnknown;  // kUnknown = autodetect
  // Exterior cell the camera starts in. When the user does not pass --cell the
  // engine substitutes a content-dense cell for the detected game (Whiterun for
  // Skyrim, New Atlantis for Starfield); start_cell_explicit pins the override.
  i32 start_cell_x = 5;
  i32 start_cell_y = -3;
  bool start_cell_explicit = false;
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
  // Server: directory of streamable UGC resources (FiveM-style). Each immediate
  // subdirectory is a resource catalogued and offered to joining clients. Empty
  // leaves asset streaming off.
  std::string mods_dir;
  // Client: where streamed mod content is cached, content-addressed. Empty falls
  // back to "recreation_asset_cache" beside the working directory. A connecting
  // client always streams the host's mods into this cache.
  std::string asset_cache_dir;
};

// A dynamic physics body mirrored into an ECS transform after each step.
struct PhysicsEntity {
  physics::BodyId body;
  ecs::Entity entity;
};

// Shared services the engine subsystems read through, plus the cross-cutting
// frame state (the walk-mode player view) several of them consume. The engine
// owns this and the systems it created; pointers to late-built services
// (assets/streamer/scripts/bindings/net) are filled in once those exist, so a
// subsystem must only touch them from code that runs after game data loads.
struct EngineContext {
  const EngineConfig* config = nullptr;

  // The detected primary game, set once after data loads. Subsystems read it to
  // pick game-specific asset layouts (e.g. the actor system's body rig).
  bethesda::Game game = bethesda::Game::kUnknown;

  // Always-present services (engine members; addresses stable for its lifetime).
  ecs::World* world = nullptr;
  ecs::Scheduler* scheduler = nullptr;
  render::Renderer* renderer = nullptr;
  FlyCamera* camera = nullptr;
  physics::PhysicsWorld* physics = nullptr;
  asset::Vfs* vfs = nullptr;
  audio::AudioSystem* audio = nullptr;
  bethesda::RecordStore* records = nullptr;
  bethesda::StringTable* strings = nullptr;
  dialogue::DialogueDb* dialogue = nullptr;
  world::QuestWorld* quest_world = nullptr;
  DebugUi* debug_ui = nullptr;
  GameUi* game_ui = nullptr;
  base::Vector<PhysicsEntity>* physics_entities = nullptr;
  // Resolved per-frame action state (owned by the host); gameplay subsystems poll
  // it for their verbs. Used by the first-person equip key.
  const ActionState* actions = nullptr;

  // Late-built services, null until the engine creates them.
  asset::AssetDatabase* assets = nullptr;
  world::CellStreamer* streamer = nullptr;
  script::ScriptSystem* scripts = nullptr;
  rx::script::skyrim::RecordBackedSkyrimBindings* bindings = nullptr;
  script::host::ManagedHost* managed = nullptr;  // null when C# scripting is off
  ItemBridge* items = nullptr;  // item pickup/drop/persistence (built after data loads)
#if RECREATION_HAS_NET
  net::GameServerSession* server_session = nullptr;
  net::GameClientSession* client_session = nullptr;
#endif

  // Walk-mode player view, written by the engine each frame and read by the
  // interaction / quest / npc subsystems. The actor system flips walk_mode /
  // third_person / auto_walk when it spawns a walkable player.
  bool walk_mode = false;
  bool third_person = true;
  bool auto_walk = false;
  // Player is sneaking (rx crouch). Written by the player controller each frame;
  // read by future stealth gameplay / HUD. CharacterState.stance is the ECS-side
  // source of truth on the player entity.
  bool sneaking = false;
  f32 cam_yaw = 0;
  // Debug/capture hook: when finite, the player controller forces the first-person
  // look pitch to this value (radians, negative = looking down) so a scripted
  // capture can aim at the floor. 1e9 = unset (normal mouse-driven pitch).
  f32 debug_look_pitch = 1e9f;
  Vec3 walk_eye{};
  Vec3 walk_target{};
  // Where the auto-walk test player should head: the active quest marker / guide
  // mark, set by the npc director each frame. When unset, auto-walk coasts along
  // the facing. Lets the guided playthrough follow the quest instead of walking
  // blindly forward into whatever happens to be ahead.
  bool auto_walk_has_goal = false;
  Vec3 auto_walk_goal{};

  // Demo scenes that stage their own lighting set this so the day/night clock
  // stops re-driving sun direction/intensity/ambient every frame (RX_SUN_DIR
  // has the same effect globally).
  bool scene_owns_sun = false;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_ENGINE_CONTEXT_H_
