#include "engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <base/option.h>

#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/record.h"
#include "core/log.h"
#include "engine_internal.h"
#include "script/papyrus/value.h"
#include "weather/weather_loader.h"
#include "world/components.h"

// Bringing a universe online: mounts archives, loads the record/string/dialogue
// data, stands up the Papyrus guest and Skyrim bindings, then builds the cell
// streamer(s) for the primary worldspace and every secondary content domain.
// Also the standalone interior and glTF scene load paths.
namespace rec {

// Load-time env overrides, declared at file scope so they register before
// InitOptionsFromEnv() populates them; see each use site for behaviour.
static base::Option<const char*> Weather{"weather", nullptr, "REC_WEATHER"};
static base::Option<bool> CwFieldBattle{"cw.field.battle", false, "REC_CW_FIELD_BATTLE"};
static base::Option<bool> CwSiegeDemo{"cw.siege.demo", false, "REC_CW_SIEGE_DEMO"};
static base::Option<bool> CwReinfTest{"cw.reinf.test", false, "REC_CW_REINF_TEST"};
static base::Option<bool> WarMap{"war.map", false, "REC_WAR_MAP"};
static base::Option<bool> VenueProbe{"venue.probe", false, "REC_VENUE_PROBE"};
static base::Option<const char*> QuestList{"quest.list", nullptr, "REC_QUEST_LIST"};
static base::Option<const char*> QuestReport{"quest.report", nullptr, "REC_QUEST_REPORT"};
static base::Option<const char*> DialogueReport{"dialogue.report", nullptr, "REC_DIALOGUE_REPORT"};
static base::Option<const char*> DialogueProbe{"dialogue.probe", nullptr, "REC_DIALOGUE_PROBE"};
static base::Option<const char*> SceneReport{"scene.report", nullptr, "REC_SCENE_REPORT"};
static base::Option<const char*> ScenePlay{"scene.play", nullptr, "REC_SCENE_PLAY"};
static base::Option<const char*> SceneLive{"scene.live", nullptr, "REC_SCENE_LIVE"};
static base::Option<const char*> DomainOffset{"domain.offset", nullptr, "REC_DOMAIN_OFFSET"};
static base::Option<const char*> DomainCell{"domain.cell", nullptr, "REC_DOMAIN_CELL"};
static base::Option<bool> AudioDump{"audio.dump", false, "REC_AUDIO_DUMP",
                                    "resolve and decode each region's ambient bed at load and log it"};

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
  self->script_bindings_->set_audio(self->audio_.get());  // Sound.Play routes here
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

  // Weather: parse the game's WTHR/CLMT into a climate and drive our physical
  // sky/clouds/atmosphere from it (no baked skydome). REC_WEATHER forces a kind.
  {
    std::unordered_map<u64, rec::weather::WeatherDef> weathers;
    const int n = rec::weather::LoadWeathers(self->records_, &weathers);
    int kinds[4] = {};
    for (auto& [id, def] : weathers) kinds[static_cast<int>(def.kind)]++;
    REC_INFO("weather: WTHR kinds -- pleasant {} cloudy {} rainy {} snow {}", kinds[0], kinds[1],
             kinds[2], kinds[3]);
    const bool starfield = self->game_ == bethesda::Game::kStarfield;
    const char* worldspace = self->game_ == bethesda::Game::kSkyrimSe  ? "Tamriel"
                             : self->game_ == bethesda::Game::kFallout4 ? "Commonwealth"
                             : starfield                                ? "NewAtlantis"
                                                                        : "";
    // Starfield authors one characteristic weather per planet worldspace, so a
    // single-weather CLMT is the real answer (don't fall through to synthetic).
    auto climate =
        rec::weather::BuildClimate(self->records_, weathers, worldspace, starfield ? 1 : 4);
    // For now, always spawn under clear weather: pin a single weather kind (clear
    // by default; REC_WEATHER overrides) and suppress the dynamic per-region
    // cycle. Set kPinClearWeather to false to restore BuildClimate's dynamic
    // weather + regions (REC_WEATHER then only pins when explicitly set).
    constexpr bool kPinClearWeather = true;
    if (kPinClearWeather || Weather.get()) {
      std::string s = Weather.get() ? Weather.get() : "clear";
      rec::weather::WeatherDef forced;
      forced.kind = (s == "rain" || s == "rainy")    ? rec::weather::WeatherDef::Kind::kRainy
                    : (s == "snow")                  ? rec::weather::WeatherDef::Kind::kSnow
                    : (s == "cloud" || s == "cloudy") ? rec::weather::WeatherDef::Kind::kCloudy
                                                      : rec::weather::WeatherDef::Kind::kPleasant;
      forced.DeriveFromKind();
      climate = {{forced, 1}};
      REC_INFO("weather: pinned to '{}' (clear by default; REC_WEATHER overrides)", s);
    }
    self->default_climate_ = climate;  // restored when the player leaves all regions
    self->weather_.SetClimate(std::move(climate));
    self->weather_.set_seed(0xBEE71Eull ^ static_cast<u64>(self->game_));
    self->ap_base_ = self->renderer_.settings().aerial_perspective;
    REC_INFO("weather: {} WTHR records, climate {} entries", n, self->weather_.size());

    // Per-region weather: the REGN areas override the worldspace climate where
    // the player stands (skipped while a single weather is pinned).
    if (!kPinClearWeather && !Weather && *worldspace) {
      const bethesda::GlobalFormId ws = self->records_.FindWorldspace(worldspace);
      const int rn = rec::weather::LoadRegions(self->records_, weathers, ws, &self->regions_);
      REC_INFO("weather: {} weather regions in {}", rn, worldspace);
    }
  }

  // Ambient audio: catalogue the game's sound files (SOUN/SNDR) and the regions'
  // ambient sound lists + areas (REGN), then point the director at them. The
  // ambient regions are resolved independently of the weather system (which the
  // engine may pin off), so this scopes them to the primary worldspace itself.
  // Built only when an audio device is live, so a muted run or a dedicated server
  // skips the scan.
  if (self->audio_ && self->audio_->active()) {
    const char* ambient_worldspace = self->game_ == bethesda::Game::kSkyrimSe  ? "Tamriel"
                                     : self->game_ == bethesda::Game::kFallout4 ? "Commonwealth"
                                     : self->game_ == bethesda::Game::kStarfield ? "NewAtlantis"
                                                                                 : "";
    const bethesda::GlobalFormId ws =
        *ambient_worldspace ? self->records_.FindWorldspace(ambient_worldspace)
                            : bethesda::GlobalFormId{};
    self->sound_catalog_.Build(self->records_);
    self->region_ambience_.Build(self->records_, ws);
    self->ambient_director_.Configure(self->audio_.get(), &self->sound_catalog_,
                                      &self->region_ambience_);

    // audio.dump (REC_AUDIO_DUMP): resolve and decode each region's ambient bed
    // up front and log the result. A no-GPU way to confirm the SOUN/SNDR/REGN
    // parse, the Vfs path resolution and the decoder line up against real data.
    if (AudioDump) {
      int ok = 0, missing = 0, undecodable = 0;
      for (u64 region : self->region_ambience_.RegionForms()) {
        for (const bethesda::GlobalFormId& snd : self->region_ambience_.SoundsFor(region)) {
          const std::string path = self->sound_catalog_.PathFor(snd);
          if (path.empty()) continue;
          if (!self->audio_->HasAsset(path)) {
            REC_INFO("audio dump: region {:x} sound {:x} -> {} (missing)", region, snd.packed(), path);
            ++missing;
            continue;
          }
          const u32 voice = self->audio_->PlayLoop(path, {});
          REC_INFO("audio dump: region {:x} -> {} ({})", region, path,
                   voice ? "decoded" : "decode failed");
          if (voice) {
            self->audio_->Stop(voice, 0.0f);
            ++ok;
          } else {
            ++undecodable;
          }
          break;  // one playable bed per region is enough
        }
      }
      REC_INFO("audio dump: {} regions playable, {} missing files, {} undecodable", ok, missing,
               undecodable);
    }
  }

  // Skyrim's northern lights: the night-sky aurora (other games don't have it).
  self->renderer_.settings().aurora = self->game_ == bethesda::Game::kSkyrimSe;

  self->quest_world_.set_on_move_player([self](u64 dest_ref, f32 x, f32 y, f32 z) {
    // The open-field battle demo anchors the player on its staged dry venue, so a
    // background quest (a follower/pet setup, an auto-started scene) must not warp
    // the player off it mid-demo.
    if (CwFieldBattle || CwSiegeDemo) return;
    // When a quest warps the player to a reference inside an interior cell (the
    // Helgen keep, say), stream that cell first so the player lands in a loaded
    // world rather than at interior-local coordinates floating in the exterior.
    // Only when the streamer actually follows the player (walk mode): in the
    // default fly-camera mode the camera, not the player, anchors streaming, so a
    // background start-game quest moving the (unfollowed) player must not unload
    // the exterior and strand the camera in an interior.
    if (self->ctx_.walk_mode && dest_ref != 0 && self->streamer_) {
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
  // Run engine-triggered stage fragments on a fiber, so a latent Wait inside one
  // suspends like a script-triggered fragment instead of returning at once.
  self->script_bindings_->set_fiber_runner(
      [guest = &self->scripts_->guest()](std::function<void()> body) {
        guest->RunScript(std::move(body));
      });
  // Keep each suspended fragment's provenance (quest + recursion depth) fiber-local
  // across a Wait that lets a different fragment run: a fresh activation starts from
  // the baseline, and a resumed one gets its own values back.
  {
    auto* binds = self->script_bindings_.get();
    self->scripts_->guest().set_fiber_context_hooks(
        [binds] {
          binds->set_active_quest(0);
          binds->set_fragment_depth(0);
        },
        [binds]() -> std::function<void()> {
          const u64 quest = binds->active_quest();
          const int depth = binds->fragment_depth();
          return [binds, quest, depth] {
            binds->set_active_quest(quest);
            binds->set_fragment_depth(depth);
          };
        });
  }
  // Hand the bindings the guest VM so quest stage fragments can execute (run on
  // the guest thread, where the bindings live). Also route the guest's engine-
  // raised events (OnActivate, OnTrigger*) through the bindings' ref->alias index,
  // so alias scripts hear them like the bindings' own native-raised events do.
  {
    auto* binds = self->script_bindings_.get();
    auto* guest = &self->scripts_->guest();
    guest->Submit([binds, guest](rec::script::papyrus::VirtualMachine& vm) {
      binds->set_vm(&vm);
      guest->set_alias_resolver(
          [binds](rec::script::papyrus::ObjectRef ref) { return binds->AliasesFilledBy(ref); });
    });
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
      guest->set_on_debug_command([self](const std::string& verb, const std::string& arg) {
        std::lock_guard<std::mutex> lock(self->debug_cmd_mutex_);
        self->pending_debug_cmds_.emplace_back(verb, arg);
      });
      guest->set_game_time_provider([self]() { return self->clock_.game_days(); });
      guest->set_los_provider([self](u64 viewer, u64 target) {
        return self->script_bindings_->HasLos(rec::script::papyrus::ObjectRef{viewer},
                                              rec::script::papyrus::ObjectRef{target});
      });
      guest->set_on_platform_hud(
          [self](const std::string& type, const std::string& func,
                 const std::vector<rec::script::papyrus::Value>& args) {
            self->platform_hud_.Submit(type, func, args);
          });
      guest->set_local_pos_provider([self]() { return self->platform_hud_.LocalPos(); });
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
  // REC_WAR_MAP opens the Civil War war-map overlay at load (normally toggled
  // with M), for screenshots.
  if (WarMap) self->war_map_open_ = true;
  if (const char* want = QuestList.get()) {
    self->quest_->ReportQuestList(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = QuestReport.get()) {
    self->quest_->ReportQuestToCompletion(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (CwReinfTest) {
    self->quest_->ReportReinforcementTest();
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = DialogueReport.get()) {
    self->quest_->ReportDialogue(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  // REC_DIALOGUE_PROBE=<0xNpcOrAchrForm> logs the topics an NPC would offer (the
  // speaker-gated, priority-ordered set), to verify dialogue without the UI.
  if (const char* want = DialogueProbe.get()) {
    self->interaction_->ReportDialogueWith(std::strtoull(want, nullptr, 0));
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = SceneReport.get()) {
    self->quest_->ReportSceneFragments(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = ScenePlay.get()) {
    self->quest_->ReportScenePlay(want);
    self->quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = SceneLive.get()) {
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
  // Runtime terrain splatting is the high-detail path; the low-end preset (no
  // ray tracing) falls back to the cheaper per-cell albedo bake.
  settings.terrain_splat = self->config_.preset != render::QualityPreset::kLowEnd;
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
  // The Civil War battle showcases stage two armies in the open, so without an
  // explicit --cell drop them onto a flat, dry patch of the Whiterun tundra
  // (cell 4,1 probed at 1.4 m max-drop, fully dry) rather than the default 5,-3
  // clifftop where the fight slides 48 m down onto the lake below.
  if (!self->config_.start_cell_explicit && self->game_ == bethesda::Game::kSkyrimSe &&
      (CwSiegeDemo || CwFieldBattle)) {
    self->config_.start_cell_x = 4;
    self->config_.start_cell_y = 1;
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
  self->showcase_regions_.push_back(
      {{start.x, ground, start.z}, std::string(profile.name), self->streamer_.get()});
  // REC_VENUE_PROBE rates the spawn cell for staging a field battle: how flat the
  // ground is within a 14 m ring and whether any of it is under water. A good
  // showcase venue has a small max-drop and zero submerged samples. Headless +
  // fast, so a range of --cell values can be screened to pick a dry, flat one.
  if (VenueProbe && self->streamer_) {
    f32 max_drop = 0;
    int submerged = 0, sampled = 0;
    for (int i = 0; i < 12; ++i) {
      const f32 a = static_cast<f32>(i) * 30.0f * 0.0174533f;
      const f32 px = start.x + std::sin(a) * 14.0f, pz = start.z - std::cos(a) * 14.0f;
      f32 h = ground;
      if (self->streamer_->GroundHeight(px, pz, &h)) {
        max_drop = std::max(max_drop, std::abs(h - ground));
        ++sampled;
      }
      f32 wh;
      Vec3 flow;
      if (self->streamer_->WaterHeightAt({px, h, pz}, &wh, &flow) && h < wh - 0.5f) ++submerged;
    }
    REC_INFO("venue probe cell {},{}: base_y={:.1f} max_drop={:.1f}m submerged={}/12 sampled={}",
             self->config_.start_cell_x, self->config_.start_cell_y, ground, max_drop, submerged,
             sampled);
  }
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
  if (const char* env = DomainOffset.get()) {
    std::sscanf(env, "%f,%f,%f", &step.x, &step.y, &step.z);
  }
  // REC_DOMAIN_CELL forces the region for every secondary; otherwise each game
  // defaults to a content-dense cell of its own (the primary camera's raw
  // coordinates usually land in empty ocean in the secondary world).
  i32 forced_x = 0, forced_y = 0;
  const bool forced =
      DomainCell.get() &&
      std::sscanf(DomainCell.get(), "%d,%d", &forced_x, &forced_y) == 2;
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
          {{place.x, place.y - 10.0f, place.z}, std::string(domain.profile().name), streamer.get()});
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
      guest->set_on_debug_command([self](const std::string& verb, const std::string& arg) {
        std::lock_guard<std::mutex> lock(self->debug_cmd_mutex_);
        self->pending_debug_cmds_.emplace_back(verb, arg);
      });
      guest->set_game_time_provider([self]() { return self->clock_.game_days(); });
      guest->set_los_provider([self](u64 viewer, u64 target) {
        return self->script_bindings_->HasLos(rec::script::papyrus::ObjectRef{viewer},
                                              rec::script::papyrus::ObjectRef{target});
      });
      guest->set_on_platform_hud(
          [self](const std::string& type, const std::string& func,
                 const std::vector<rec::script::papyrus::Value>& args) {
            self->platform_hud_.Submit(type, func, args);
          });
      guest->set_local_pos_provider([self]() { return self->platform_hud_.LocalPos(); });
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
