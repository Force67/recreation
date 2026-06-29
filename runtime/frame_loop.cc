#include "engine.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

#include <base/option.h>

#include "bethesda/record.h"
#include "core/log.h"
#include "core/math.h"
#include "core/types.h"
#include "engine_internal.h"
#include "script/papyrus/value.h"
#include "world/cell_streaming.h"
#include "world/components.h"
#include "world/interaction.h"
#include "world/objective_marker.h"

// The Engine's per-frame heartbeat: the fixed-step simulation loop, the
// quest-driven world mutation flush, and host-authoritative NPC shove-out, plus
// the render path that builds the FrameView and submits it. Split out of the
// core lifecycle unit so the hot loop reads on its own.
namespace rec {

// Case-insensitive ASCII string compare, for matching a NetEntity model against a
// record's editor id.
static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

// Config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
static base::Option<float> Lightning{"lightning", 0.0f, "REC_LIGHTNING"};
static base::Option<const char*> UiShot{"ui.shot", nullptr, "REC_UI_SHOT"};
static base::Option<int> UiShotFrames{"ui.shot.frames", 30, "REC_UI_SHOT_FRAMES"};

void Engine::ApplyQuestWorld() {
  std::vector<world::WorldCommand> commands = quest_world_queue_.Drain();
  if (commands.empty()) return;
  quest_world_.Apply(commands);  // host/single-player: apply locally + record provenance
#if RECREATION_HAS_NET
  if (server_session_) server_session_->SendWorldCommands(commands);  // mirror to clients
#endif
}

void Engine::ServerSimulateActors(f32 /*dt*/) {
  // Pushers: the local player (listen server / single-player) plus every
  // networked player. A player that is both contributes twice, which is
  // harmless (the second shove is a no-op once the first cleared the radius).
  base::Vector<Vec3> pushers;
  const ecs::Entity local = actors_->PlayerEntity();
  if (world_.IsAlive(local))
    if (const world::Transform* t = world_.Get<world::Transform>(local))
      pushers.push_back({t->position[0], t->position[1], t->position[2]});
#if RECREATION_HAS_NET
  world_.Each<net::NetworkId, world::Transform>(
      [&](ecs::Entity, net::NetworkId&, world::Transform& t) {
        pushers.push_back({t.position[0], t.position[1], t.position[2]});
      });
#endif
  if (pushers.empty()) return;

  constexpr f32 kPushRadius = 0.6f;  // ~capsule radius in meters
  world_.Each<world::Npc, world::Transform>([&](ecs::Entity, world::Npc&, world::Transform& nt) {
    for (const Vec3& p : pushers) {
      const float pusher[3] = {p.x, p.y, p.z};
      float out[3];
      if (world::ShoveOutOfRadius(pusher, nt.position, kPushRadius, out)) {
        nt.position[0] = out[0];
        nt.position[1] = out[1];
        nt.position[2] = out[2];
      }
    }
  });
}

bool Engine::RunFrame() {
  if (quit_.load(std::memory_order_relaxed)) return false;
#if RECREATION_HAS_NET
  // Apply a requested live mod reload here, before any stage reads the Vfs.
  if (mod_reload_requested_.exchange(false, std::memory_order_relaxed)) ReloadMods(*this);
#endif
  if (window_ && !window_->PumpEvents()) return false;
  // Resolve this pump's raw keyboard/mouse + gamepad state into semantic
  // actions for the gameplay/camera/menu code to read.
  if (window_) input_map_.Resolve(window_->input(), window_->gamepad(), &actions_);
  // Forward key presses to the managed world (KeyPressed) so mods can bind
  // hotkeys, unless the debug console is capturing the keyboard. Queued here and
  // drained into managed below, in the same frame.
  if (window_ && managed_ && !debug_ui_.wants_keyboard()) {
    const InputState& keys = window_->input();
    for (u8 k = 0; k < static_cast<u8>(Key::kCount); ++k)
      if (keys.key_pressed(static_cast<Key>(k)))
        managed_->QueueEvent({rec::script::host::ManagedEventId::kKeyPressed, k, 0, 0, 0.0f});
  }
  {
    int steps = timer_.Tick();
    f32 dt = static_cast<f32>(timer_.fixed_step());
    // Advance the in-world clock (day/night cycle) by the real frame time. The
    // Papyrus time natives read it through the bindings; the render path below
    // derives the sun and sky tint from it.
    clock_.Advance(timer_.frame_delta());
    // Place NPC / other-player collision capsules at their current transforms
    // before the sim step, so the player's character controller collides with
    // them where they are this frame.
    actors_->SyncSolidBodies();
    for (int i = 0; i < steps; ++i) {
      scheduler_.RunStage(ecs::Stage::kPreSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kPostSim, world_, dt);
    }

    // The guest advances on the main loop's clock; it does its work on its own
    // thread, so this only posts a tick.
    if (scripts_) scripts_->Tick(static_cast<f32>(timer_.frame_delta()));
    // Secondary game domains tick their own isolated microvms in lockstep.
    for (auto& domain : extra_domains_) domain->Tick(static_cast<f32>(timer_.frame_delta()));
    // Refresh the world-space position snapshot the managed proximity query reads
    // (registered refs plus the player), so mods see this frame's positions.
    if (managed_ && ctx_.bindings) {
      quest_world_.SnapshotPositions(position_snapshot_);
      Vec3 pp;
      if (actors_->PlayerWorldPos(&pp)) position_snapshot_.push_back({0x14, {pp.x, pp.y, pp.z}});
      ctx_.bindings->UpdatePositionSnapshot(position_snapshot_);
    }
    // Advance the managed world: deliver any queued engine events to mod hooks,
    // then run the per-frame behaviours (Skyrim soft logic), all dispatching back
    // into the engine through the bridge.
    if (managed_) {
      managed_->DrainEvents();
      managed_->Tick(static_cast<f32>(timer_.frame_delta()));
    }
    // Advance any scenes a quest fragment Started; the ScenePlayer fires their
    // phase fragments over time (host-authoritative; runs on the guest thread).
#if RECREATION_HAS_NET
    if (scripts_ && ctx_.bindings && !client_session_) {
#else
    if (scripts_ && ctx_.bindings) {
#endif
      auto* binds = ctx_.bindings;
      const f32 sdt = static_cast<f32>(timer_.frame_delta());
      scripts_->guest().Submit(
          [binds, sdt](rec::script::papyrus::VirtualMachine&) { binds->TickScenes(sdt); });
    }

    // Apply (and, when hosting, replicate) the world mutations quests requested
    // on the guest thread. Main-thread only, so it owns the ECS exclusively here.
    ApplyQuestWorld();

    // Authoritative NPC simulation runs on the host / single-player only; a
    // client receives the results via actor sync instead of simulating.
#if RECREATION_HAS_NET
    if (!client_session_) ServerSimulateActors(static_cast<f32>(timer_.frame_delta()));
#else
    ServerSimulateActors(static_cast<f32>(timer_.frame_delta()));
#endif
    // Steer follower NPCs toward the player and scene guides toward their
    // targets (host authoritative; streams to clients via actor sync).
    npc_->UpdateFollowers(static_cast<f32>(timer_.frame_delta()));
    npc_->UpdateGuides(static_cast<f32>(timer_.frame_delta()));
    npc_->UpdateAmbient(static_cast<f32>(timer_.frame_delta()));  // idle sandbox for streamed NPCs
    // Combat enrollment from the guest thread (StartCombat/StopCombat/death),
    // then drive the melee simulation (host/single-player authoritative).
    for (const world::CombatEvent& e : combat_event_queue_.Drain()) {
      switch (e.op) {
        case world::CombatOp::kEngage: npc_->EnterCombat(e.actor, e.target); break;
        case world::CombatOp::kDisengage: npc_->LeaveCombat(e.actor); break;
        case world::CombatOp::kDied: npc_->OnActorDied(e.actor); break;
        case world::CombatOp::kResurrected: npc_->OnActorResurrected(e.actor); break;
      }
    }
    npc_->Cw00DemoTick(static_cast<f32>(timer_.frame_delta()));
    npc_->CwBattleTick(static_cast<f32>(timer_.frame_delta()));
    npc_->CwFieldBattleTick(static_cast<f32>(timer_.frame_delta()));
    npc_->UpdateCombat(static_cast<f32>(timer_.frame_delta()));
    // Mirror any soldiers the battle spawned to clients (so they render the same
    // bipeds); the actor sync then streams their movement. Drained every frame so
    // single-player simply discards them.
    {
      std::vector<world::WorldCommand> spawns = npc_->DrainReplicatedSpawns();
#if RECREATION_HAS_NET
      if (server_session_ && !spawns.empty()) server_session_->SendWorldCommands(spawns);
#endif
    }
    npc_->Mq101DemoTick(static_cast<f32>(timer_.frame_delta()));
    npc_->Mq101SceneTick(static_cast<f32>(timer_.frame_delta()));
    // World-driven progression: the player walking into a scripted trigger box
    // fires its OnTriggerEnter, the native way Skyrim advances a quest.
    interaction_->UpdateTriggers();

    if (!config_.headless) {
      f32 frame_delta = static_cast<f32>(timer_.frame_delta());
      // Weather, parsed from the game's WTHR/CLMT, drives our physical sky/clouds
      // (never its baked skydome). Cloud coverage + aerial-perspective haze update
      // every frame (cheap, no IBL rebuild); the sun tint/dimming folds into the
      // throttled day/night update below.
      // Region weather: the REGN area the player stands in overrides the
      // worldspace climate (Skyrim's per-region weather). Swapped only when the
      // active region changes; skipped while the debug panel overrides weather.
      if (!weather_override_ && !regions_.empty()) {
        Vec3 anchor = camera_.position();
        Vec3 ppos;
        if (ctx_.walk_mode && actors_->PlayerWorldPos(&ppos)) anchor = ppos;
        constexpr f32 kEngineToGame = 1.0f / 0.01428f;  // metres -> Bethesda units
        u64 region = 0;
        const auto* climate =
            regions_.ClimateAt(anchor.x * kEngineToGame, -anchor.z * kEngineToGame, &region);
        if (region != active_region_) {
          // Capture the weather we are leaving (old climate) to cross-fade from.
          region_blend_from_ =
              weather_.empty() ? weather::WeatherState{} : weather_.At(clock_.game_days());
          region_blend_t_ = 0.0f;
          active_region_ = region;
          weather_.SetClimate(climate ? *climate : default_climate_);
          REC_INFO("weather: region {:x} ({} weathers)", region,
                   climate ? climate->size() : default_climate_.size());
        }
      }
      // The debug Weather panel can override the climate live; otherwise the
      // loaded game's weather drives the sky.
      const bool has_weather = weather_override_ || !weather_.empty();
      weather::WeatherState w;
      if (weather_override_)
        w = weather_override_state_;
      else if (!weather_.empty())
        w = weather_.At(clock_.game_days());
      // Cross-fade over ~5 s when the region changed, so weather eases in.
      if (!weather_override_ && region_blend_t_ < 1.0f) {
        region_blend_t_ =
            std::min(1.0f, region_blend_t_ + static_cast<f32>(timer_.frame_delta()) / 5.0f);
        const f32 s = region_blend_t_ * region_blend_t_ * (3.0f - 2.0f * region_blend_t_);
        w = weather::Lerp(region_blend_from_, w, s);
      }
      if (has_weather) {
        renderer_.settings().cloud_coverage = w.cloud_coverage;
        renderer_.settings().aerial_perspective = ap_base_ * (1.0f + w.aerosol * 2.0f);
        // No rain/snow (or wetness) indoors: interior cells have no sky overhead.
        const bool indoors = streamer_ && streamer_->in_interior();
        renderer_.settings().precipitation = indoors ? 0.0f : w.precipitation;
        renderer_.settings().precip_snow = w.snow;
      }
      // Thunderstorm lightning: a decaying flash (with a flicker) scheduled at
      // random intervals while a thundery weather is active (heavy rain, FO4
      // radstorms). The weather sets w.thunder; rain isn't required (dust storms).
      {
        const bool indoors = streamer_ && streamer_->in_interior();
        const bool storm = has_weather && !indoors && w.thunder;
        const f32 now = clock_.real_hours() * 3600.0f;
        if (!storm) {
          lightning_ = 0.0f;
          next_strike_ = now + 3.0f;  // first possible strike once a storm begins
        } else {
          if (now >= next_strike_) {
            strike_time_ = now;
            lightning_seed_ = lightning_seed_ * 1664525u + 1013904223u;
            const f32 r = static_cast<f32>(lightning_seed_ >> 8) / 16777216.0f;
            next_strike_ = now + 4.0f + r * 13.0f;  // 4-17 s apart
          }
          const f32 e = now - strike_time_;
          const f32 a = std::exp(-e * 9.0f);                                       // main flash
          const f32 b = e > 0.12f ? 0.65f * std::exp(-(e - 0.12f) * 12.0f) : 0.0f;  // flicker
          lightning_ = std::min(1.0f, a + b);
        }
        // REC_LIGHTNING holds the flash at a fixed level (testing the brief, random strike).
        if (Lightning.overridden()) lightning_ = Lightning.get();
        renderer_.settings().lightning = lightning_;
      }
      // Day/night: derive the sun direction/intensity/color/ambient from the
      // clock's time of day (unless REC_SUN_DIR pinned a fixed sun), tinted and
      // dimmed by the weather. Throttled to ~0.02-hour steps so the IBL
      // environment is not rebuilt every frame, also re-firing when the weather
      // light changes.
      if (drive_sun_from_clock_) {
        const f32 hour = clock_.hour();
        const bool weather_dirty = std::abs(w.light_scale - last_weather_scale_) > 0.01f ||
                                   std::abs(w.light_tint.x - last_weather_tint_.x) > 0.01f ||
                                   std::abs(w.light_tint.y - last_weather_tint_.y) > 0.01f ||
                                   std::abs(w.light_tint.z - last_weather_tint_.z) > 0.01f;
        if (last_sky_hour_ < -100.0f || std::abs(hour - last_sky_hour_) >= 0.02f || weather_dirty) {
          last_sky_hour_ = hour;
          last_weather_scale_ = w.light_scale;
          last_weather_tint_ = w.light_tint;
          const SkyLighting sky = ComputeSkyLighting(hour);
          auto& s = renderer_.settings();
          s.sun_direction = sky.sun_direction;
          s.sun_intensity = sky.sun_intensity * w.light_scale;
          s.sun_color = {sky.sun_color.x * w.light_tint.x, sky.sun_color.y * w.light_tint.y,
                         sky.sun_color.z * w.light_tint.z};
          s.ambient = sky.ambient + (has_weather ? w.cloud_coverage * 0.05f : 0.0f);
        }
      }
      TickMenuCapture();  // grab a clean backdrop frame after entering a universe
      debug_ui_.BeginFrame();
      UpdateCamera(frame_delta);
      UpdateSettings();  // pause-menu controls: rebind capture + sensitivity
      actors_->SyncNpcActors();  // add/remove NPC actors as cells stream in/out
      actors_->Update(frame_delta);
      scheduler_.RunStage(ecs::Stage::kPreRender, world_, frame_delta);

      render::FrameView view;
      if (ctx_.walk_mode && actors_->HasPlayer()) {
        view.camera.eye = ctx_.walk_eye;
        view.camera.target = ctx_.walk_target;
      } else {
        view.camera.eye = camera_.position();
        view.camera.target = camera_.target();
      }
      view.frame_delta_seconds = frame_delta;
      // Move the audio listener to this frame's viewpoint (the walk-mode player or
      // the fly camera), so positional voices pan and attenuate around the player.
      if (audio_) {
        const Vec3 eye = view.camera.eye;
        const Vec3 forward = Normalize(view.camera.target - eye);
        audio_->SetListener(eye, forward, Vec3{0, 1, 0});
      }
      // Rebuilt every frame so destroyed entities drop out on their own.
      base::UnorderedMap<u64, Mat4> transforms;
      world_.Each<world::Transform, world::Renderable>(
          [&](ecs::Entity entity, world::Transform& transform, world::Renderable& renderable) {
            if (world_.Has<world::Hidden>(entity)) return;  // Disable()d by a quest
            u64 key = static_cast<u64>(entity.generation) << 32 | entity.index;
            Mat4 current = TransformMatrix(transform);
            const Mat4* prev = prev_transforms_.find(key);
            view.draws.push_back({renderable.mesh.hash, current, prev ? *prev : current});
            transforms.insert(key, current);
          });
      prev_transforms_ = std::move(transforms);
      actors_->EmitDraws(view);
      demos_->EmitToView(frame_delta, view);
      if (editor_) editor_->CollectLights(view.lights);  // placed torches/lamps light the scene
      quest_->RefreshQuestPanel(frame_delta);
      quest_->RefreshNativeTrace(frame_delta);
      // Outside a scripted playthrough, the auto-walk test player heads for the
      // tracked quest's current objective, so it walks the real route (and trips
      // the world triggers along it) rather than wandering forward.
      if (ctx_.auto_walk && !npc_->guiding()) {
        Vec3 goal;
        ctx_.auto_walk_has_goal = quest_->CurrentObjectiveTarget(&goal);
        if (ctx_.auto_walk_has_goal) ctx_.auto_walk_goal = goal;
      }
      debug_ui_.Build(renderer_, camera_, frame_delta, &view, quest_->quest_panel(),
                      quest_->native_trace_panel());
      // Drain queued Debug.Notification messages onto the HUD toast.
      {
        std::vector<std::string> notifications;
        {
          std::lock_guard<std::mutex> lock(notification_mutex_);
          notifications.swap(pending_notifications_);
        }
        for (const std::string& message : notifications) game_ui_.FlashQuestUpdate(message);
      }
      // Drain the multiplayer platform HUD: notices flash on the toast, chat
      // accumulates into the chat box (kept to a bounded tail), and a Net.Connect
      // intent is logged for the front-end to act on.
      for (const PlatformNotice& n : platform_hud_.DrainNotices())
        game_ui_.FlashQuestUpdate(n.text);
      for (const PlatformChatLine& c : platform_hud_.DrainChat())
        platform_chat_display_.push_back(c.name.empty() ? c.text : c.name + ": " + c.text);
      constexpr size_t kChatDisplayCap = 64;
      if (platform_chat_display_.size() > kChatDisplayCap)
        platform_chat_display_.erase(
            platform_chat_display_.begin(),
            platform_chat_display_.end() - static_cast<std::ptrdiff_t>(kChatDisplayCap));
      game_ui_.SetChatLines(platform_chat_display_);
      // Scoreboard: format each player's cells into rough fixed-width columns (the
      // first cell, the name, gets the widest field) and hand the panel the lines.
      {
        const PlatformScoreboard sb = platform_hud_.Scoreboard();
        auto pad = [](const std::string& s, size_t width) {
          std::string out = s;
          if (out.size() < width) out.append(width - out.size(), ' ');
          return out;
        };
        auto format_cells = [&pad](const std::vector<std::string>& cells) {
          std::string line;
          for (size_t i = 0; i < cells.size(); ++i) line += pad(cells[i], i == 0 ? 26 : 12);
          return line;
        };
        std::vector<std::string> rows;
        rows.reserve(sb.rows.size());
        for (const PlatformScoreRow& r : sb.rows) rows.push_back(format_cells(r.cells));
        game_ui_.SetScoreboard(sb.open, sb.title, format_cells(sb.headers), rows);
      }
      // Interaction prompts: format each as "[KEY]  label" for the bottom stack.
      {
        std::vector<std::string> prompts;
        for (const PlatformPrompt& p : platform_hud_.Prompts()) {
          std::string line;
          if (!p.key.empty()) line += "[" + p.key + "]  ";
          line += p.label;
          prompts.push_back(line);
        }
        game_ui_.SetPrompts(prompts);
      }
      // Publish the local viewer's world position (engine space) so mods can place
      // things relative to the player through the Net.LocalPos* natives. The camera
      // is the render-space viewpoint the player rides, which is what projects.
      {
        const Vec3 p = camera_.position();
        platform_hud_.SetLocalPos(p.x, p.y, p.z);
      }
      // Map blips on the compass: turn each blip's world position into a bearing
      // from where the player looks, dropping ones behind or off the strip. Blip
      // positions are engine space (placed via Net.LocalPos), matching the camera.
      {
        const Vec3 eye = camera_.position();
        const Vec3 tgt = camera_.target();
        const float fwd[3] = {tgt.x - eye.x, 0.0f, tgt.z - eye.z};
        std::vector<GameUi::CompassBlip> cblips;
        for (const PlatformBlip& b : platform_hud_.Blips()) {
          const float to[3] = {b.x - eye.x, 0.0f, b.z - eye.z};
          const float bearing = world::MarkerCompassBearingDeg(fwd, to);
          if (std::fabs(bearing) > 105.0f) continue;  // behind the player / off the strip
          cblips.push_back({bearing, b.color ? b.color : 0xffffffffu});
          if (cblips.size() >= 8) break;
        }
        game_ui_.SetCompassBlips(cblips);
      }
      if (std::optional<std::string> addr = platform_hud_.TakePendingConnect())
        REC_INFO("[platform] connect requested: {}", *addr);
      // Floating player nametags: project each world-space label to screen pixels
      // for the 2D HUD, dropping ones behind the camera or off-screen.
      {
        const std::vector<PlatformNametag> tags = platform_hud_.DrainNametags();
        std::vector<GameUi::Nametag> screen_tags;
        const Vec3 eye = camera_.position();
        const Vec3 cf = camera_.forward();
        const Vec3 cr = Normalize(Cross(cf, Vec3{0, 1, 0}));
        const Vec3 cu = Cross(cr, cf);
        const f32 w = static_cast<f32>(renderer_.output_width());
        const f32 h = static_cast<f32>(renderer_.output_height());
        const f32 aspect = h > 0 ? w / h : 1.0f;
        const f32 tan_half = std::tan(1.0472f * 0.5f);  // kFovY 60deg
        for (const PlatformNametag& n : tags) {
          const Vec3 to{n.x - eye.x, n.y - eye.y, n.z - eye.z};
          const f32 zc = Dot(to, cf);
          if (zc <= 0.1f) continue;  // behind the camera
          const f32 ndc_x = (Dot(to, cr) / zc) / (tan_half * aspect);
          const f32 ndc_y = (Dot(to, cu) / zc) / tan_half;
          if (std::fabs(ndc_x) > 1.2f || std::fabs(ndc_y) > 1.2f) continue;  // off screen
          screen_tags.push_back({n.label, (ndc_x * 0.5f + 0.5f) * w, (0.5f - ndc_y * 0.5f) * h,
                                 n.color ? n.color : 0xffffffffu});
          if (screen_tags.size() >= 16) break;
        }
        game_ui_.SetNametags(screen_tags);
      }
      // Networked entities: apply a mod's spawn/move/delete onto the ECS world.
      // Resolve a default placeholder base form (a compact static) once; a spawn
      // with no model, or an unknown one, falls back to it.
      if (!net_entity_base_ready_) {
        net_entity_base_ready_ = true;
        records_.EachOfType(
            FourCc('S', 'T', 'A', 'T'),
            [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
              if (net_entity_base_.local_id != 0) return;  // first good hit wins
              bethesda::Record record;
              if (!records_.Parse(id, &record)) return;
              const bethesda::Subrecord* modl = record.Find(FourCc('M', 'O', 'D', 'L'));
              if (!modl || modl->data.empty()) return;
              std::string path(reinterpret_cast<const char*>(modl->data.data()), modl->data.size());
              if (size_t z = path.find('\0'); z != std::string::npos) path.resize(z);
              for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
              // Need a real world mesh; skip markers, effects and the non-world art
              // (load screens, UI) that carry a model but never render in the cell.
              if (path.find(".nif") == std::string::npos) return;
              for (const char* bad : {"marker", "fx\\", "loadscreen", "interface", "effects\\"}) {
                if (path.find(bad) != std::string::npos) return;
              }
              // Prefer a compact, recognizable prop for the placeholder; otherwise
              // take the first acceptable static found.
              const bool nice = path.find("barrel") != std::string::npos ||
                                path.find("crate") != std::string::npos ||
                                path.find("rock") != std::string::npos ||
                                path.find("boulder") != std::string::npos;
              if (!nice && net_entity_base_fallback_.local_id == 0)
                net_entity_base_fallback_ = id;
              if (nice) net_entity_base_ = id;
            });
        if (net_entity_base_.local_id == 0) net_entity_base_ = net_entity_base_fallback_;
      }
      for (const PlatformEntityOp& op : platform_hud_.DrainEntityOps()) {
        if (op.kind == PlatformEntityOp::Kind::kSpawn) {
          if (!streamer_) continue;
          // Resolve the requested model (an editor id) to a base form so a mod
          // spawns a specific object by name; fall back to the placeholder static
          // when it is empty or unknown. Cached, since the lookup scans records.
          bethesda::GlobalFormId form = net_entity_base_;
          if (!op.model.empty()) {
            auto cached = net_model_cache_.find(op.model);
            if (cached != net_model_cache_.end()) {
              form = cached->second;
            } else {
              bethesda::GlobalFormId resolved{};
              for (u32 type : {FourCc('S', 'T', 'A', 'T'), FourCc('M', 'S', 'T', 'T'),
                               FourCc('F', 'U', 'R', 'N'), FourCc('M', 'I', 'S', 'C')}) {
                if (resolved.local_id != 0) break;
                records_.EachOfType(type, [&](bethesda::GlobalFormId id,
                                              const bethesda::RecordStore::StoredRecord&) {
                  if (resolved.local_id != 0) return;
                  bethesda::Record r;
                  if (records_.Parse(id, &r) && EqualsIgnoreCase(r.GetString(FourCc('E', 'D', 'I', 'D')), op.model))
                    resolved = id;
                });
              }
              if (resolved.local_id == 0) resolved = net_entity_base_;  // unknown -> placeholder
              net_model_cache_[op.model] = resolved;
              form = resolved;
            }
          }
          // Place through the cell streamer at the entity's engine-space world
          // position. PlaceObject adds Transform + Renderable, so the object is
          // drawn by the normal frame draw pass above.
          const Vec3 pos{op.x, op.y, op.z};
          const f32 rot[4] = {0, 0, 0, 1};
          ecs::Entity e = streamer_->PlaceObject(world_, form, pos, rot, 1.0f);
          if (e != ecs::kInvalidEntity) net_entities_.insert(op.id, e);
        } else if (op.kind == PlatformEntityOp::Kind::kMove) {
          if (ecs::Entity* e = net_entities_.find(op.id)) {
            if (world::Transform* t = world_.Get<world::Transform>(*e)) {
              t->position[0] = op.x;
              t->position[1] = op.y;
              t->position[2] = op.z;
            }
          }
        } else if (op.kind == PlatformEntityOp::Kind::kDelete) {
          if (ecs::Entity* e = net_entities_.find(op.id)) {
            world_.Destroy(*e);
            net_entities_.erase(op.id);
          }
        }
      }
      // Surface managed HUD gauges (oxygen, radiation, ...) pushed via Hud.Gauge
      // by the primary game's gameplay systems.
      if (script_bindings_) {
        std::vector<script::skyrim::RecordBackedSkyrimBindings::HudGauge> gauges;
        script_bindings_->SnapshotHudGauges(gauges);
        std::vector<HudGauge> hud;
        hud.reserve(gauges.size());
        for (const auto& g : gauges) hud.push_back({g.id, g.label, g.fraction, g.color});
        // A staged battle adds two reinforcement bars (the modern read-out of who
        // is winning). The real Civil War siege drives its own bars from the pool
        // globals via the managed BattleReinforcementHud; this covers the engine's
        // own staged battles.
        f32 f1, f2;
        if (npc_->BattleGauges(&f1, &f2)) {
          hud.push_back({"cw_team1", "Imperial line", f1, 0x4f7fd8ffu});      // blue
          hud.push_back({"cw_team2", "Stormcloak line", f2, 0xd84f4fffu});    // red
        }
        game_ui_.SetHudGauges(hud);

        // War map (M): the managed Civil War campaign pushes each hold's owner;
        // snapshot it onto the toggle-able panel.
        std::vector<script::skyrim::RecordBackedSkyrimBindings::WarHold> war_holds;
        f32 war_progress = 0.0f;
        script_bindings_->SnapshotWarMap(war_holds, war_progress);
        std::vector<GameUi::WarHoldEntry> holds;
        holds.reserve(war_holds.size());
        for (const auto& h : war_holds) holds.push_back({h.name, h.owner});
        game_ui_.SetWarMap(war_map_open_, holds, war_progress);
      }
      game_ui_.Build(*window_, renderer_, camera_, frame_delta, &view);
      renderer_.RenderFrame(view);
      // Test/CI hook: REC_UI_SHOT=<path> grabs the frame after REC_UI_SHOT_FRAMES
      // (default 30) and quits. Lets a headless GPU run capture the UI without
      // loading a universe (the NEXUS menu renders at boot).
      if (const char* shot = UiShot.get()) {
        static int ui_shot_frames = 0;
        static const int ui_shot_target = [] {
          return UiShotFrames.get() > 0 ? UiShotFrames.get() : 30;
        }();
        ++ui_shot_frames;
        // CaptureScreenshot is deferred: it is written by the NEXT RenderFrame.
        // Request at the target frame, then quit one frame later so the write
        // actually lands.
        if (ui_shot_frames == ui_shot_target) renderer_.CaptureScreenshot(shot);
        else if (ui_shot_frames > ui_shot_target) quit_.store(true, std::memory_order_relaxed);
      }
    } else {
      // No vsync to pace the loop; yield between fixed steps instead of
      // spinning a core.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return !quit_.load(std::memory_order_relaxed);
}

}  // namespace rec
