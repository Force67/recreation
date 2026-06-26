#include "engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>

#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/record.h"
#include "core/log.h"
#include "engine_internal.h"
#include "script/papyrus/value.h"
#include "world/components.h"

// Bringing a universe online: mounts archives, loads the record/string/dialogue
// data, stands up the Papyrus guest and Skyrim bindings, then builds the cell
// streamer(s) for the primary worldspace and every secondary content domain.
// Also the standalone interior and glTF scene load paths.
namespace rec {

bool LoadGameData(Engine& engine) {
  Engine* const self = &engine;
  self->game_ = self->config_.game != bethesda::Game::kUnknown
              ? self->config_.game
              : bethesda::GameProfile::DetectFromDataDir(self->config_.data_dir);
  if (self->game_ == bethesda::Game::kUnknown) {
    REC_ERROR("could not detect a supported game in {}", self->config_.data_dir);
    return false;
  }
  self->ctx_.game = self->game_;
  const auto& profile = bethesda::GameProfile::For(self->game_);
  REC_INFO("detected {}", profile.name);

  MountArchives(engine);
  // Loose files mount last so they win over archives.
  self->vfs_.Mount(asset::MakeLooseFileProvider(self->config_.data_dir));

  self->assets_ = std::make_unique<asset::AssetDatabase>(self->vfs_);
  self->ctx_.assets = self->assets_.get();
  bethesda::RegisterConverters(*self->assets_, profile);

  auto order = bethesda::LoadOrder::FromPluginsTxt(self->config_.plugins_txt, profile);
  if (!self->records_.LoadAll(self->config_.data_dir, order, profile)) return false;
  REC_INFO("{} plugins, {} records", order.plugins().size(), self->records_.record_count());

  // Localized string tables, base masters first so their ids win the collisions
  // a single id-keyed table cannot avoid (the main quest text lives in the base
  // game master). Plugins without string files (non-localized) are skipped.
  for (const std::string& plugin : order.plugins())
    self->strings_.Load(self->vfs_, plugin, profile.string_language);
  REC_INFO("loaded {} localized strings", self->strings_.size());

  // Index dialogue topics by quest so an NPC conversation opens without
  // rescanning every DIAL.
  self->dialogue_.Build(self->records_);
  REC_INFO("dialogue: {} topics indexed", self->dialogue_.topic_count());

  // The Papyrus guest: a separate, single-threaded world that runs game scripts
  // off the main thread. Form natives read the RecordStore; actor values and
  // inventory are backed by the bindings' own stores.
  self->script_bindings_ = std::make_unique<rec::script::skyrim::RecordBackedSkyrimBindings>(&self->records_);
  self->ctx_.bindings = self->script_bindings_.get();
  self->script_bindings_->set_strings(&self->strings_);
  self->script_bindings_->set_player(rec::script::papyrus::ObjectRef{0x14});  // Skyrim player ref
  // Route quest-driven world mutations (PlaceAtMe/MoveTo/Enable/Delete + cleanup)
  // through the provenance layer; the player teleports through a host hook since
  // it is an actor/capsule, not a registry entity.
  self->script_bindings_->set_world_sink(&self->runtime_world_sink_);
  // Day/night clock: map the time globals (resolved by editor id, so it works
  // across Skyrim/Fallout/Starfield) onto the clock, then reseed it with the
  // game's authored TimeScale (Bethesda default 20), honouring the env overrides.
  self->script_bindings_->set_clock(&self->clock_);
  const auto game_hour = self->records_.FindGlobal("GameHour");
  const auto days_passed = self->records_.FindGlobal("GameDaysPassed");
  auto timescale_glob = self->records_.FindGlobal("TimeScale");
  if (timescale_glob.plugin == 0xffff) timescale_glob = self->records_.FindGlobal("Timescale");
  f32 authored_timescale = 20.0f;
  if (timescale_glob.plugin != 0xffff) {
    // Read before set_time_globals so it returns the authored FLTV, not the clock.
    authored_timescale = self->script_bindings_->GetGlobalValue(
        rec::script::papyrus::ObjectRef{timescale_glob.packed()});
  }
  self->ConfigureClock(authored_timescale);
  self->script_bindings_->set_time_globals(game_hour.plugin == 0xffff ? 0 : game_hour.packed(),
                                           days_passed.plugin == 0xffff ? 0 : days_passed.packed(),
                                           timescale_glob.plugin == 0xffff ? 0
                                                                           : timescale_glob.packed());
  self->quest_world_.set_on_move_player([self](u64 dest_ref, f32 x, f32 y, f32 z) {
    // When a quest warps the player to a reference inside an interior cell (the
    // Helgen keep, say), stream that cell first so the player lands in a loaded
    // world rather than at interior-local coordinates floating in the exterior.
    if (dest_ref != 0 && self->streamer_) {
      const bethesda::GlobalFormId ref{static_cast<u16>(dest_ref >> 32),
                                       static_cast<u32>(dest_ref & 0xffffffffu)};
      const bethesda::GlobalFormId interior = self->records_.InteriorCellOfRef(ref);
      if (interior.plugin != 0xffff) {
        Vec3 spawn;
        if (self->streamer_->EnterInterior(self->world_, interior, &spawn))
          REC_INFO("quest: entered interior {:04x}:{:06x} to move the player", interior.plugin,
                   interior.local_id);
      } else if (self->streamer_->in_interior()) {
        self->streamer_->EnterExterior(self->world_);  // a move back out to the worldspace
      }
    }
    self->actors_->TeleportPlayer(x, y, z);
  });
  // A connecting client is a passive replica: the server runs the scripts and is
  // authoritative for quest and quest-driven world state; the client mirrors it
  // through replicated quest snapshots and world commands. Definitions still load
  // (for journal text), but the client's own scripts may not mutate that state.
  // The host and single-player stay authoritative.
  self->script_bindings_->set_replica_mode(!self->config_.connect_address.empty());
  if (self->script_bindings_->replica_mode())
    REC_INFO("multiplayer client: quests run server-authoritative (replica mode)");
  self->scripts_ = std::make_unique<rec::script::ScriptSystem>(self->game_, &self->vfs_, self->script_bindings_.get());
  self->ctx_.scripts = self->scripts_.get();
  // Hand the bindings the guest VM so quest stage fragments can execute (run on
  // the guest thread, where the bindings live).
  {
    auto* binds = self->script_bindings_.get();
    self->scripts_->guest().Submit(
        [binds](rec::script::papyrus::VirtualMachine& vm) { binds->set_vm(&vm); });
  }
  // Route Debug.Notification (from Papyrus quests or C# mods) to the HUD toast.
  // Set on the guest thread so it never races the native; the handler only queues,
  // and the main loop drains it to the UI.
  {
    auto* guest = &self->scripts_->guest();
    guest->Submit([guest, self](rec::script::papyrus::VirtualMachine&) {
      guest->set_on_notification([self](const std::string& message) {
        std::lock_guard<std::mutex> lock(self->notification_mutex_);
        self->pending_notifications_.push_back(message);
      });
    });
  }
  self->quest_->AttachQuestScripts();

  // Load any additional games as live secondary domains before the managed world
  // boots, so C# mods see every game's content at once.
  LoadExtraDomains(engine);

  // Bring up the managed (C#) scripting world over the same guest, so user mods
  // and Skyrim soft logic run alongside Papyrus. Optional and gracefully absent.
  BootManagedScripting(engine);

  // REC_QUEST_REPORT=<EDID> drives a quest through its stages to completion and
  // prints the journey, then quits; REC_DIALOGUE_REPORT dumps its dialogue.
  if (const char* want = std::getenv("REC_QUEST_REPORT")) {
    self->quest_->ReportQuestToCompletion(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_DIALOGUE_REPORT")) {
    self->quest_->ReportDialogue(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_SCENE_REPORT")) {
    self->quest_->ReportSceneFragments(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_SCENE_PLAY")) {
    self->quest_->ReportScenePlay(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_SCENE_LIVE")) {
    self->quest_->ReportSceneLive(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  // Headless reports drive quests during init and quit before the main loop, so
  // deliver any events they queued to managed hooks here too.
  if (self->managed_) self->managed_->DrainEvents();

  // Actor bringup scene: load a Skyrim character and animate it, no streaming.
  if (self->config_.demo_scene == "actor") return self->actors_->CreateSkyrimActor();

  self->streamer_ = std::make_unique<world::CellStreamer>(self->records_, *self->assets_);
  self->ctx_.streamer = self->streamer_.get();
  // Forward load-door cell transitions to the managed world (LocationChanged), so
  // mods react to where the player is. Runs on the main thread; drained next frame.
  if (self->managed_) {
    auto* host = self->managed_.get();
    self->streamer_->set_on_location_change([host](u64 cell, bool interior) {
      host->QueueEvent(
          {rec::script::host::ManagedEventId::kLocationChanged, cell, 0, interior ? 1 : 0, 0.0f});
    });
  }
  // Register streamed NPCs in the quest world so quests can target them and
  // clients can apply replicated actor transforms by form id.
  self->streamer_->set_quest_world(&self->quest_world_);
  if (self->physics_.initialized()) {
    self->streamer_->set_physics(&self->physics_);
    self->physics_.set_water_height([self](const Vec3& position, f32* height, Vec3* flow) {
      return self->streamer_->WaterHeightAt(position, height, flow);
    });
  }
  world::CellStreamer::Settings settings;
  settings.grass_density = self->config_.grass_density;
  self->streamer_->Configure(settings);
  if (!self->config_.headless) {
    world::CellStreamer::Uploads uploads;
    uploads.mesh = [self](const asset::Mesh& mesh) { return self->renderer_.UploadMesh(mesh); };
    uploads.texture = [self](const asset::Texture& texture) {
      return self->renderer_.UploadTexture(texture);
    };
    uploads.material = [self](const asset::Material& material) {
      return self->renderer_.UploadMaterial(material);
    };
    self->streamer_->SetUploads(std::move(uploads));
  }

  if (!self->config_.interior.empty()) return LoadInterior(engine);
  if (!self->streamer_->SelectWorldspace(profile.exterior_worldspace)) return false;

  // Without an explicit --cell, start in the game's content-dense cell so the
  // first view is its signature locale, not whichever grid Whiterun happens to
  // share. (0,-3) is the busiest New Atlantis exterior cell.
  if (!self->config_.start_cell_explicit && self->game_ == bethesda::Game::kStarfield) {
    self->config_.start_cell_x = 0;
    self->config_.start_cell_y = -3;
  }

  // Drop the camera a bit above the terrain at the middle of the start cell.
  constexpr f32 kUnitsToMeters = 0.01428f;
  constexpr f32 kCellSize = 4096.0f;
  f32 beth_x = (static_cast<f32>(self->config_.start_cell_x) + 0.5f) * kCellSize;
  f32 beth_y = (static_cast<f32>(self->config_.start_cell_y) + 0.5f) * kCellSize;
  Vec3 start{beth_x * kUnitsToMeters, 0.0f, -beth_y * kUnitsToMeters};
  f32 ground = 0;
  if (self->streamer_->GroundHeight(start.x, start.z, &ground)) {
    start.y = ground + 10.0f;  // a little above the terrain for a view
  } else {
    REC_WARN("no terrain at start cell {},{}", self->config_.start_cell_x, self->config_.start_cell_y);
  }
  self->camera_.set_position(start);
  self->camera_.set_yaw_pitch(0.0f, -0.1f);
  self->camera_.speed = 30.0f;
  REC_INFO("camera start: cell {},{} at ({:.1f}, {:.1f}, {:.1f})", self->config_.start_cell_x,
           self->config_.start_cell_y, start.x, start.y, start.z);
  self->actors_->MaybeSpawnWorldPlayer({start.x, ground, start.z});  // on the terrain, not 10m up
  self->showcase_regions_.push_back({{start.x, ground, start.z}, std::string(profile.name)});
  SetupExtraStreamers(engine);

  // Tell the editor which games' assets it can place: the primary, then each
  // secondary content domain (so a Fallout 4 prop can be dropped into Skyrim).
  if (self->editor_) {
    std::vector<EditorPlaceDomain> domains;
    domains.push_back(
        {std::string(profile.name), GameSlug(self->game_), &self->records_, &self->strings_, self->streamer_.get()});
    for (size_t i = 0; i < self->extra_domains_.size() && i < self->extra_streamers_.size(); ++i) {
      ContentDomain& d = *self->extra_domains_[i];
      domains.push_back({std::string(d.profile().name), GameSlug(d.profile().game), &d.records(),
                         &d.strings(), self->extra_streamers_[i].get()});
    }
    self->editor_->SetPlaceDomains(std::move(domains));
  }
  return true;
}

void SetupExtraStreamers(Engine& engine) {
  Engine* const self = &engine;
  if (self->config_.headless || self->extra_domains_.empty()) return;
  constexpr f32 kUnitsToMeters = 0.01428f;
  constexpr f32 kCellSize = 4096.0f;

  // Each secondary worldspace is a fixed diorama placed this far east of the
  // primary camera, stepped per domain so several never overlap. REC_DOMAIN_OFFSET
  // tunes the seam; REC_DOMAIN_CELL="x,y" picks which region of the secondary
  // world to show (its default is a content-dense cell, not the empty ocean the
  // primary camera's own coordinates would land on).
  Vec3 step{450.0f, 0.0f, 0.0f};
  if (const char* env = std::getenv("REC_DOMAIN_OFFSET")) {
    std::sscanf(env, "%f,%f,%f", &step.x, &step.y, &step.z);
  }
  // REC_DOMAIN_CELL forces the region for every secondary; otherwise each game
  // defaults to a content-dense cell of its own (the primary camera's raw
  // coordinates usually land in empty ocean in the secondary world).
  i32 forced_x = 0, forced_y = 0;
  const bool forced =
      std::getenv("REC_DOMAIN_CELL") &&
      std::sscanf(std::getenv("REC_DOMAIN_CELL"), "%d,%d", &forced_x, &forced_y) == 2;
  const Vec3 cam = self->camera_.position();

  for (size_t i = 0; i < self->extra_domains_.size(); ++i) {
    ContentDomain& domain = *self->extra_domains_[i];
    // A content-dense default cell per game (Whiterun for Skyrim, the
    // Commonwealth coast for Fallout 4); the forced override wins when set.
    i32 region_x = -18, region_y = 7;  // Commonwealth coast: highway, rocks, trees
    if (domain.profile().game == bethesda::Game::kSkyrimSe) {
      region_x = 5;
      region_y = -3;  // Whiterun
    } else if (domain.profile().game == bethesda::Game::kStarfield) {
      region_x = 0;
      region_y = -3;  // New Atlantis: the busiest exterior cell
    }
    if (forced) {
      region_x = forced_x;
      region_y = forced_y;
    }
    auto streamer = std::make_unique<world::CellStreamer>(domain.records(), domain.assets());

    // Namespace this domain's mesh ids so they cannot collide with the primary
    // game's (shared asset paths hash the same). A large odd multiplier keeps
    // the salts distinct and non-zero. The streamer salts its Renderable ids;
    // the upload callback below salts the matching renderer/BLAS keys.
    const u64 salt = 0x9E3779B97F4A7C15ull * static_cast<u64>(i + 1);
    streamer->set_mesh_id_salt(salt);

    // The region's center in the secondary world's own (pre-offset) engine space.
    f32 region_bx = (static_cast<f32>(region_x) + 0.5f) * kCellSize;
    f32 region_by = (static_cast<f32>(region_y) + 0.5f) * kCellSize;
    Vec3 anchor{region_bx * kUnitsToMeters, 0.0f, -region_by * kUnitsToMeters};
    streamer->set_fixed_anchor(anchor);
    // Offset so the region center lands `step*(i+1)` from the primary camera,
    // at the camera's height by default so the secondary world sits beside the
    // primary instead of floating far above or sunk below it.
    Vec3 place{cam.x + step.x * static_cast<f32>(i + 1), cam.y + step.y * static_cast<f32>(i + 1),
               cam.z + step.z * static_cast<f32>(i + 1)};
    streamer->set_world_offset({place.x - anchor.x, place.y - anchor.y, place.z - anchor.z});

    world::CellStreamer::Settings settings;
    settings.grass_density = self->config_.grass_density;
    streamer->Configure(settings);
    world::CellStreamer::Uploads uploads;
    uploads.mesh = [self, salt](const asset::Mesh& mesh) {
      return self->renderer_.UploadMesh(mesh, salt);
    };
    uploads.texture = [self, salt](const asset::Texture& texture) {
      return self->renderer_.UploadTexture(texture, salt);
    };
    uploads.material = [self, salt](const asset::Material& material) {
      return self->renderer_.UploadMaterial(material, salt);
    };
    streamer->SetUploads(std::move(uploads));
    if (streamer->SelectWorldspace(domain.profile().exterior_worldspace)) {
      REC_INFO("secondary worldspace rendering: {} cell {},{} placed at ({:.0f}, {:.0f}, {:.0f})",
               domain.profile().name, region_x, region_y, place.x, place.y, place.z);
      // The showcase flies over each rendered region; its ground baseline sits the
      // same 10m below the placed camera-height anchor as the primary's does.
      self->showcase_regions_.push_back(
          {{place.x, place.y - 10.0f, place.z}, std::string(domain.profile().name)});
    } else {
      REC_WARN("secondary domain {} has no worldspace '{}': not rendered, assets still placeable",
               domain.profile().name, domain.profile().exterior_worldspace);
    }
    // Kept parallel to extra_domains_ even when not rendered, so the editor can
    // place this game's assets (PlaceObject needs the per-domain streamer/salt).
    self->extra_streamers_.push_back(std::move(streamer));
  }
}

bool LoadInterior(Engine& engine) {
  Engine* const self = &engine;
  bethesda::GlobalFormId cell_id;
  if (self->config_.interior.starts_with("0x") || self->config_.interior.starts_with("0X")) {
    // Load order form id: top byte is the plugin index for full plugins.
    u32 raw = static_cast<u32>(std::stoul(self->config_.interior.substr(2), nullptr, 16));
    cell_id = {static_cast<u16>(raw >> 24), raw & 0xffffff};
  } else {
    cell_id = self->records_.FindInteriorCell(self->config_.interior);
  }
  if (cell_id.plugin == 0xffff) {
    REC_ERROR("interior cell not found: {}", self->config_.interior);
    return false;
  }

  Vec3 start{};
  if (!self->streamer_->LoadInterior(self->world_, cell_id, &start)) return false;
  self->camera_.set_position(start);
  self->camera_.set_yaw_pitch(0.0f, 0.0f);
  self->camera_.speed = 5.0f;
  REC_INFO("camera start: interior {} at ({:.1f}, {:.1f}, {:.1f})", self->config_.interior, start.x,
           start.y, start.z);
  REC_INFO("interior {}: {} npcs loaded", self->config_.interior, self->streamer_->spawned_npc_count());
  self->actors_->MaybeSpawnWorldPlayer(start);
  return true;
}

void LoadExtraDomains(Engine& engine) {
  Engine* const self = &engine;
  for (const ExtraDomainConfig& cfg : self->config_.extra_domains) {
    auto domain = std::make_unique<ContentDomain>();
    // A multiplayer client mirrors the host, so secondary domains are replicas
    // there too: their scripts read content but do not drive authoritative state.
    if (!domain->Load(cfg.game, cfg.data_dir, cfg.plugins_txt,
                      /*replica_mode=*/!self->config_.connect_address.empty())) {
      REC_WARN("secondary domain failed to load: {}", cfg.data_dir);
      continue;
    }
    // Surface its Debug.Notification on the same HUD toast as the primary game.
    auto* guest = &domain->scripts()->guest();
    guest->Submit([guest, self](rec::script::papyrus::VirtualMachine&) {
      guest->set_on_notification([self](const std::string& message) {
        std::lock_guard<std::mutex> lock(self->notification_mutex_);
        self->pending_notifications_.push_back(message);
      });
    });
    // Run that game's quests inside its own microvm (capped like the primary).
    domain->AttachQuestScripts(self->config_.max_quest_scripts);
    REC_INFO("secondary domain live: {} ({} records, isolated microvm)", domain->profile().name,
             domain->records().record_count());
    self->extra_domains_.push_back(std::move(domain));
  }
}

void MountArchives(Engine& engine) {
  Engine* const self = &engine;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(self->config_.data_dir, ec)) {
    std::string path = entry.path().string();
    // TODO: archive order should follow plugin order plus the ini resource
    // lists, alphabetical is a placeholder.
    if (auto provider = bethesda::OpenArchive(path)) self->vfs_.Mount(std::move(provider));
  }
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

}  // namespace rec
