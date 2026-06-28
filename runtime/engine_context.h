#ifndef RECREATION_RUNTIME_ENGINE_CONTEXT_H_
#define RECREATION_RUNTIME_ENGINE_CONTEXT_H_

#include <string>

#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "asset/vfs.h"
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
#include "render/presets.h"
#include "render/renderer.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/host/managed_host.h"
#include "script/script_system.h"
#include "world/cell_streaming.h"
#include "world/quest_world.h"

#if RECREATION_HAS_NET
#include "net/session.h"
#endif

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

  // Always-present services (engine members; addresses stable for its lifetime).
  ecs::World* world = nullptr;
  ecs::Scheduler* scheduler = nullptr;
  render::Renderer* renderer = nullptr;
  FlyCamera* camera = nullptr;
  physics::PhysicsWorld* physics = nullptr;
  asset::Vfs* vfs = nullptr;
  bethesda::RecordStore* records = nullptr;
  bethesda::StringTable* strings = nullptr;
  dialogue::DialogueDb* dialogue = nullptr;
  world::QuestWorld* quest_world = nullptr;
  DebugUi* debug_ui = nullptr;
  GameUi* game_ui = nullptr;
  base::Vector<PhysicsEntity>* physics_entities = nullptr;

  // Late-built services, null until the engine creates them.
  asset::AssetDatabase* assets = nullptr;
  world::CellStreamer* streamer = nullptr;
  script::ScriptSystem* scripts = nullptr;
  rec::script::skyrim::RecordBackedSkyrimBindings* bindings = nullptr;
  script::host::ManagedHost* managed = nullptr;  // null when C# scripting is off
#if RECREATION_HAS_NET
  net::ServerSession* server_session = nullptr;
  net::ClientSession* client_session = nullptr;
#endif

  // Walk-mode player view, written by the engine each frame and read by the
  // interaction / quest / npc subsystems. The actor system flips walk_mode /
  // third_person / auto_walk when it spawns a walkable player.
  bool walk_mode = false;
  bool third_person = true;
  bool auto_walk = false;
  f32 cam_yaw = 0;
  Vec3 walk_eye{};
  Vec3 walk_target{};
  // Where the auto-walk test player should head: the active quest marker / guide
  // mark, set by the npc director each frame. When unset, auto-walk coasts along
  // the facing. Lets the guided playthrough follow the quest instead of walking
  // blindly forward into whatever happens to be ahead.
  bool auto_walk_has_goal = false;
  Vec3 auto_walk_goal{};
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_ENGINE_CONTEXT_H_
