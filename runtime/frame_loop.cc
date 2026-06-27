#include "engine.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

#include "core/log.h"
#include "engine_internal.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/interaction.h"

// The Engine's per-frame heartbeat: the fixed-step simulation loop, the
// quest-driven world mutation flush, and host-authoritative NPC shove-out, plus
// the render path that builds the FrameView and submits it. Split out of the
// core lifecycle unit so the hot loop reads on its own.
namespace rec {

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
      if (has_weather) {
        renderer_.settings().cloud_coverage = w.cloud_coverage;
        renderer_.settings().aerial_perspective = ap_base_ * (1.0f + w.aerosol * 2.0f);
        // No rain/snow (or wetness) indoors: interior cells have no sky overhead.
        const bool indoors = streamer_ && streamer_->in_interior();
        renderer_.settings().precipitation = indoors ? 0.0f : w.precipitation;
        renderer_.settings().precip_snow = w.snow;
      }
      // Thunderstorm lightning: a decaying flash (with a flicker) scheduled at
      // random intervals while heavy rain falls.
      {
        const bool indoors = streamer_ && streamer_->in_interior();
        const bool storm = has_weather && !indoors && w.precipitation > 0.5f && !w.snow;
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
        if (const char* fl = std::getenv("REC_LIGHTNING")) lightning_ = static_cast<f32>(std::atof(fl));
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
      // Surface managed HUD gauges (oxygen, radiation, ...) pushed via Hud.Gauge
      // by the primary game's gameplay systems.
      if (script_bindings_) {
        std::vector<script::skyrim::RecordBackedSkyrimBindings::HudGauge> gauges;
        script_bindings_->SnapshotHudGauges(gauges);
        std::vector<HudGauge> hud;
        hud.reserve(gauges.size());
        for (const auto& g : gauges) hud.push_back({g.id, g.label, g.fraction, g.color});
        game_ui_.SetHudGauges(hud);
      }
      game_ui_.Build(*window_, renderer_, camera_, frame_delta, &view);
      renderer_.RenderFrame(view);
      // Test/CI hook: REC_UI_SHOT=<path> grabs the frame after REC_UI_SHOT_FRAMES
      // (default 30) and quits. Lets a headless GPU run capture the UI without
      // loading a universe (the NEXUS menu renders at boot).
      if (const char* shot = std::getenv("REC_UI_SHOT")) {
        static int ui_shot_frames = 0;
        static const int ui_shot_target = [] {
          const char* f = std::getenv("REC_UI_SHOT_FRAMES");
          return f && std::atoi(f) > 0 ? std::atoi(f) : 30;
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
