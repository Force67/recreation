#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/record.h"
#include "core/log.h"
#include "core/math.h"
#include "engine_internal.h"
#include "quest/quest_def.h"
#include "script/host/managed_host.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/interaction.h"

namespace rec {

// A stable per-game slug stored in the editor's layout file (defined below).
static std::string GameSlug(bethesda::Game game);

bool Engine::Initialize(const EngineConfig& config, std::unique_ptr<Window> window) {
  config_ = config;
  jobs_ = std::make_unique<JobSystem>();

  if (!config_.headless) {
    window_ = window ? std::move(window) : Window::Create({});
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
    ApplyRenderPreset();
  }

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("debug ui unavailable");
    }
    if (!game_ui_.Initialize(*window_, renderer_)) {
      REC_WARN("game ui unavailable");
    }
  }

  // Wire the shared service bundle and build the gameplay subsystems. The
  // late-built services (assets/streamer/scripts/bindings/net) are filled into
  // the context as they come up in LoadGameData / StartNetworking.
  ctx_.config = &config_;
  ctx_.world = &world_;
  ctx_.scheduler = &scheduler_;
  ctx_.renderer = &renderer_;
  ctx_.camera = &camera_;
  ctx_.physics = &physics_;
  ctx_.vfs = &vfs_;
  ctx_.records = &records_;
  ctx_.strings = &strings_;
  ctx_.dialogue = &dialogue_;
  ctx_.quest_world = &quest_world_;
  ctx_.debug_ui = &debug_ui_;
  ctx_.game_ui = &game_ui_;
  ctx_.physics_entities = &physics_entities_;
  actors_ = std::make_unique<ActorSystem>(ctx_);
  interaction_ = std::make_unique<InteractionSystem>(ctx_, actors_.get());
  npc_ = std::make_unique<NpcDirector>(ctx_, actors_.get());
  quest_ = std::make_unique<QuestDirector>(ctx_, actors_.get());
  demos_ = std::make_unique<DemoScenes>(ctx_, actors_.get());
  npc_->set_siblings(interaction_.get(), quest_.get());
  quest_->set_siblings(npc_.get(), interaction_.get());
  // The live map editor (windowed client only). Constructed after game_ui_ is up
  // so it can register its overlay event sink; ticked from UpdateCamera.
  if (!config_.headless) editor_ = std::make_unique<MapEditor>(ctx_);

  if (physics_.Initialize()) {
    // A small wooden cube every scene can throw around (F key).
    asset::Material wood;
    wood.id = asset::MakeAssetId("builtin/physics_cube/material");
    wood.base_color_factor[0] = 0.42f;
    wood.base_color_factor[1] = 0.26f;
    wood.base_color_factor[2] = 0.14f;
    wood.roughness_factor = 0.75f;
    asset::Mesh cube = asset::MakeCube(0.25f, asset::MakeAssetId("builtin/physics_cube"));
    for (asset::MeshLod& lod : cube.lods) {
      for (asset::Submesh& submesh : lod.submeshes) submesh.material = wood.id;
      if (lod.submeshes.empty()) {
        lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), wood.id});
      }
    }
    physics_cube_mesh_ = cube.id;
    if (!config_.headless) {
      renderer_.UploadMaterial(wood);
      renderer_.UploadMesh(cube);
    }
    scheduler_.AddSystem(ecs::Stage::kSim, "physics", [this](ecs::World& world, f32 dt) {
      physics_.Update(dt);
      for (const PhysicsEntity& body : physics_entities_) {
        world::Transform* transform = world.Get<world::Transform>(body.entity);
        if (!transform) continue;
        Vec3 position;
        f32 rotation[4];
        if (physics_.GetBodyTransform(body.body, &position, rotation)) {
          transform->position[0] = position.x;
          transform->position[1] = position.y;
          transform->position[2] = position.z;
          std::memcpy(transform->rotation, rotation, sizeof(rotation));
        }
      }
    });
  }

  if (!config_.gltf_path.empty()) {
    if (!LoadGltfScene()) return false;
  } else if (!config_.data_dir.empty()) {
    if (!LoadGameData()) return false;
  } else {
    demos_->CreateDemoScene();
  }

#if RECREATION_HAS_NET
  if (!StartNetworking()) return false;
#endif

  scheduler_.AddSystem(ecs::Stage::kPostSim, "cell_streaming", [this](ecs::World& world, f32) {
    if (!streamer_) return;
    // In walk mode the streamer follows the player, not the fly camera (which is
    // frozen while walking): this keeps cells loaded as the player walks far and,
    // crucially, as quest fragments and load doors teleport them across the
    // worldspace, instead of leaving them stranded in unstreamed space.
    Vec3 anchor = camera_.position();
    Vec3 ppos;
    if (ctx_.walk_mode && actors_->PlayerWorldPos(&ppos)) anchor = ppos;
    streamer_->Update(world, anchor);
    // Secondary worldspaces follow the same anchor; each applies its own offset
    // internally so it streams the region that lands beside the primary world.
    for (auto& extra : extra_streamers_) extra->Update(world, anchor);
  });

  return true;
}

#if RECREATION_HAS_NET
bool Engine::StartNetworking() {
  net::SessionConfig net_config;
  net_config.port = config_.port;
  net_config.player_name = base::NameString(config_.player_name.c_str());
  net_config.max_clients = config_.max_clients;
  // Joining players replicate as cubes until there are real actor assets.
  net_config.player_mesh = asset::MakeAssetId("builtin/cube").hash;

  if (config_.host_server) {
    auto server = std::make_unique<net::ServerSession>(std::move(net_config));
    if (!server->Start()) return false;
    server_session_ = server.get();
    ctx_.server_session = server_session_;
    session_ = std::move(server);
    // Replicate the authoritative quest journal. The source is only called when
    // clients are connected, so the guest round-trip costs nothing while idle.
    // Quest state lives on the guest thread, so we marshal the read onto it.
    if (scripts_ && script_bindings_) {
      server_session_->SetQuestSource([this]() -> std::vector<net::DomainQuestStatus> {
        // Replicate every loaded game's journal, each tagged with its domain so
        // the client routes it to the matching game.
        std::vector<net::DomainQuestStatus> all;
        auto collect = [&](u8 domain, rec::script::ScriptSystem* scripts,
                           rec::script::skyrim::RecordBackedSkyrimBindings* binds) {
          if (!scripts || !binds) return;
          auto statuses = scripts->guest()
                              .SubmitFor([binds](script::papyrus::VirtualMachine&) {
                                return binds->quest_system().AllStatuses();
                              })
                              .get();
          for (quest::QuestStatus& s : statuses) all.push_back({domain, std::move(s)});
        };
        collect(0, scripts_.get(), script_bindings_.get());
        for (size_t i = 0; i < extra_domains_.size(); ++i) {
          collect(static_cast<u8>(i + 1), extra_domains_[i]->scripts(),
                  extra_domains_[i]->bindings());
        }
        return all;
      });
      // A client activating a reference runs OnActivate authoritatively here; the
      // resulting quest/world changes replicate back through the usual channels.
      server_session_->SetActivateSink([this](u64 handle) { interaction_->RaiseActivate(handle); });
      // A client picking a dialogue topic runs that INFO's fragment here, so the
      // quest advances on the server and replicates to everyone.
      server_session_->SetDialogueSink([this](u64 info) { interaction_->RunInfoFragment(info); });
      // A client's quest debugger acts through the server: apply the requested
      // stage/objective/running change on the guest, which replicates back as a
      // normal quest update.
      server_session_->SetStageRequestSink([this](const net::StageRequest& r) {
        if (!scripts_) return;
        auto* binds = script_bindings_.get();
        scripts_->guest().Submit([binds, r](script::papyrus::VirtualMachine&) {
          const script::papyrus::ObjectRef ref{r.quest};
          switch (r.op) {
            case net::StageOp::kSetStage:
              binds->SetStage(ref, r.a);
              break;
            case net::StageOp::kSetRunning:
              if (r.b)
                binds->StartQuest(ref);
              else
                binds->StopQuest(ref);
              break;
            case net::StageOp::kSetObjectiveDisplayed:
              binds->SetObjectiveDisplayed(ref, r.a, r.b != 0);
              break;
            case net::StageOp::kSetObjectiveCompleted:
              binds->SetObjectiveCompleted(ref, r.a, r.b != 0);
              break;
          }
        });
      });
    }
    // Stream authoritative NPC transforms; the session deltas them so only the
    // NPCs that actually moved this tick go out.
    server_session_->SetActorSource([this]() { return net::CollectActorStates(world_); });
  } else if (!config_.connect_address.empty()) {
    net_config.address = base::String(config_.connect_address.c_str());
    auto client = std::make_unique<net::ClientSession>(std::move(net_config));
    if (!client->Start()) return false;
    client_session_ = client.get();
    ctx_.client_session = client_session_;
    session_ = std::move(client);
    // Mirror the server's journal onto our quest system. ApplyStatus mutates
    // quest state, so it has to run on the guest thread like every other write.
    if (scripts_ && script_bindings_) {
      client_session_->SetQuestSink([this](u8 domain, const quest::QuestStatus& status) {
        // Route the replicated quest to the game it belongs to: 0 is the primary
        // game, 1..N the secondary domains loaded in the same order as the host.
        rec::script::ScriptSystem* scripts = nullptr;
        rec::script::skyrim::RecordBackedSkyrimBindings* binds = nullptr;
        if (domain == 0) {
          scripts = scripts_.get();
          binds = script_bindings_.get();
        } else if (static_cast<size_t>(domain - 1) < extra_domains_.size()) {
          scripts = extra_domains_[domain - 1]->scripts();
          binds = extra_domains_[domain - 1]->bindings();
        }
        if (!scripts || !binds) return;
        scripts->guest().Submit([binds, status](script::papyrus::VirtualMachine&) {
          binds->quest_system().ApplyStatus(status);
        });
        if (std::getenv("REC_NET_QUEST_LOG"))
          REC_INFO("net: applied domain {} quest 0x{:x} stage {} complete {}", domain,
                   status.handle, status.stage, status.complete ? 1 : 0);
      });
      // Mirror the host's quest-driven world effects (spawns/moves/disables/
      // cleanup). Runs in the net sim stage on the main thread, which owns the
      // ECS, so applying straight to QuestWorld is safe.
      client_session_->SetWorldCommandSink(
          [this](const std::vector<world::WorldCommand>& cmds) { quest_world_.Apply(cmds); });
      // Mirror authoritative NPC movement onto our existing (cell-loaded) NPC
      // entities, interpolated between updates.
      client_session_->SetActorSink([this](const std::vector<net::ActorState>& actors) {
        net::ApplyActorStates(world_, quest_world_, actors, 0.1f);
      });
      // Show the host's active objective waypoint on our own compass: store its
      // world position; UpdateObjectiveMarkers turns it into a local bearing.
      client_session_->SetObjectiveMarkerSink([this](const net::ObjectiveMarkerState& m) {
        quest_->SetRemoteMarker(m.active, Vec3{m.x, m.y, m.z});
      });
    }
  } else {
    return true;
  }

  scheduler_.AddSystem(ecs::Stage::kSim, "net",
                       [this](ecs::World& world, f32 dt) { session_->Tick(world, dt); });
  if (client_session_) {
    // Remote transforms blend between snapshots. With a renderer that runs
    // per frame; headless clients smooth at the fixed step instead.
    const ecs::Stage stage = config_.headless ? ecs::Stage::kPostSim : ecs::Stage::kPreRender;
    scheduler_.AddSystem(stage, "net_interpolation",
                         [](ecs::World& world, f32 dt) { net::TickInterpolation(world, dt); });
  }
  return true;
}
#endif  // RECREATION_HAS_NET

void Engine::ApplyRenderPreset() {
  render::Device* device = renderer_.device();
  if (!device || device->is_stub()) return;  // no gpu, nothing to tune
  const render::DeviceCaps& caps = device->caps();
  render::QualityPreset resolved = render::ResolvePreset(config_.preset, caps);
  render::RenderSettings tuned = render::PresetSettings(resolved, caps);

  // Explicit reconstruction flags (--no-taa / --upscaler) still win over the
  // preset's choice; --no-rt already gates ray tracing at the device level.
  if (config_.renderer.aa_mode == render::AntiAliasingMode::kNone) {
    tuned.aa_mode = render::AntiAliasingMode::kNone;
    tuned.upscaler = render::UpscalerKind::kNone;
  } else if (config_.renderer.upscaler != render::UpscalerKind::kNone) {
    tuned.upscaler = config_.renderer.upscaler;
    tuned.aa_mode = render::AntiAliasingMode::kUpscaler;
  }

  // Initialize() applied the REC_DEBUG_VIEW / REC_PATHTRACE debug env overrides;
  // carry them through so a preset never silently disables headless captures.
  const render::RenderSettings& env = renderer_.settings();
  tuned.debug_view = env.debug_view;
  if (env.debug_view != render::DebugView::kOff) {
    tuned.auto_exposure = false;
    tuned.exposure = 1.0f;
  }
  if (env.path_trace) tuned.path_trace = true;
  if (env.wireframe) tuned.wireframe = true;  // honor REC_WIREFRAME over the preset
  tuned.ssr = env.ssr;                        // honor REC_SSR over the preset
  tuned.ssgi = env.ssgi;                      // honor REC_SSGI over the preset
  tuned.color_grade = env.color_grade;        // presets never set a grade
  tuned.sun_direction = env.sun_direction;    // honor REC_SUN_DIR over the default
  if (std::getenv("REC_NO_OCCLUSION")) tuned.gpu_occlusion = false;  // a/b baseline

  renderer_.settings() = tuned;
  REC_INFO("render preset: {} ({})", render::PresetName(resolved),
           config_.preset == render::QualityPreset::kAuto ? "auto" : "forced");
}

void Engine::ApplyQuestWorld() {
  std::vector<world::WorldCommand> commands = quest_world_queue_.Drain();
  if (commands.empty()) return;
  quest_world_.Apply(commands);  // host/single-player: apply locally + record provenance
  if (server_session_) server_session_->SendWorldCommands(commands);  // mirror to clients
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
  world_.Each<net::NetworkId, world::Transform>(
      [&](ecs::Entity, net::NetworkId&, world::Transform& t) {
        pushers.push_back({t.position[0], t.position[1], t.position[2]});
      });
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
  if (window_ && !window_->PumpEvents()) return false;
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
    if (scripts_ && ctx_.bindings && !client_session_) {
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
    if (!client_session_) ServerSimulateActors(static_cast<f32>(timer_.frame_delta()));
    // Steer follower NPCs toward the player and scene guides toward their
    // targets (host authoritative; streams to clients via actor sync).
    npc_->UpdateFollowers(static_cast<f32>(timer_.frame_delta()));
    npc_->UpdateGuides(static_cast<f32>(timer_.frame_delta()));
    npc_->Mq101DemoTick(static_cast<f32>(timer_.frame_delta()));
    npc_->Mq101SceneTick(static_cast<f32>(timer_.frame_delta()));
    // World-driven progression: the player walking into a scripted trigger box
    // fires its OnTriggerEnter, the native way Skyrim advances a quest.
    interaction_->UpdateTriggers();

    if (!config_.headless) {
      f32 frame_delta = static_cast<f32>(timer_.frame_delta());
      debug_ui_.BeginFrame();
      UpdateCamera(frame_delta);
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
      game_ui_.Build(*window_, renderer_, camera_, frame_delta, &view);
      renderer_.RenderFrame(view);
    } else {
      // No vsync to pace the loop; yield between fixed steps instead of
      // spinning a core.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return !quit_.load(std::memory_order_relaxed);
}

int Engine::Run() {
  while (RunFrame()) {
  }
  return 0;
}

void Engine::OnSurfaceDestroyed() {
  if (!config_.headless) renderer_.DestroySurface();
}

void Engine::OnSurfaceCreated() {
  if (!config_.headless) renderer_.RecreateSurface();
}

Engine::~Engine() { Shutdown(); }

void Engine::Shutdown() {
  if (shut_down_) return;  // idempotent: explicit Shutdown then destructor
  shut_down_ = true;
  // Run managed teardown while the guest is still alive (its shutdown callbacks
  // dispatch through the bridge), then stop the guest so no more events reach the
  // host, then destroy the host. This exact order keeps the event sink, which
  // the guest thread holds, valid until the guest is joined.
  if (managed_) managed_->Shutdown();
  scripts_.reset();
  extra_streamers_.clear();  // reference domain records/assets; drop before the domains
  extra_domains_.clear();    // joins each secondary guest thread before teardown
  managed_.reset();
  if (!config_.headless) {
    renderer_.WaitIdle();
    game_ui_.Shutdown();
    debug_ui_.Shutdown();
    renderer_.Shutdown();
  }
  if (jobs_) jobs_->WaitIdle();
}

void Engine::BootManagedScripting() {
  // A replica client mirrors the server; its scripts must not mutate
  // authoritative state, so the managed world stays off here.
  if (script_bindings_ && script_bindings_->replica_mode()) {
    REC_INFO("managed: skipped on a replica client (server-authoritative)");
    return;
  }
  const char* dir = std::getenv("RECREATION_SCRIPTING_DIR");
  if (!dir || !*dir) {
    REC_INFO("managed: RECREATION_SCRIPTING_DIR unset, C# scripting disabled");
    return;
  }
  namespace fs = std::filesystem;
  const fs::path base(dir);
  const std::string assembly = (base / "Recreation.Scripting.dll").string();
  const std::string runtime_config = (base / "Recreation.Scripting.runtimeconfig.json").string();
  std::error_code ec;
  if (!fs::exists(assembly, ec) || !fs::exists(runtime_config, ec)) {
    REC_WARN("managed: Recreation.Scripting not found under {}", dir);
    return;
  }
  managed_ = std::make_unique<rec::script::host::ManagedHost>();
  // Register the primary (rendered) game first, then every secondary domain, so a
  // C# mod can reach Skyrim and Fallout content at the same time. Each domain
  // dispatches into its own guest, keeping the games' state isolated.
  managed_->AddDomain(
      bethesda::GameProfile::For(game_).name, scripts_->guest(),
      [this](const std::string& name) { return !scripts_->EnsureScriptLoaded(name).empty(); });
  for (auto& domain : extra_domains_) {
    ContentDomain* d = domain.get();
    managed_->AddDomain(d->profile().name, d->scripts()->guest(), [d](const std::string& name) {
      return !d->scripts()->EnsureScriptLoaded(name).empty();
    });
  }
  if (!managed_->Boot(/*dotnet_root=*/"", runtime_config, assembly)) {
    managed_.reset();  // unavailable: run without it
    return;
  }
  // Route gameplay events (death, item added, quest stage) from the bindings to
  // the managed event bus. The sink runs on the guest thread, so set it there to
  // avoid racing the guest, and have it only enqueue: the main loop drains the
  // queue into managed code (DrainEvents).
  auto* host = managed_.get();
  auto* binds = script_bindings_.get();
  scripts_->guest().Submit([binds, host](rec::script::papyrus::VirtualMachine&) {
    binds->set_event_sink(
        [host](const rec::script::host::ManagedEvent& e) { host->QueueEvent(e); });
  });
  // Notify the managed world when a form's scripts attach (it goes live), so
  // mods can attach C# behaviours to it. Fires on the main thread; QueueEvent is
  // drained next frame.
  scripts_->set_on_scripts_attached([host](u64 form) {
    host->QueueEvent({rec::script::host::ManagedEventId::kFormLoaded, form, 0, 0, 0.0f});
  });
  // The symmetric unload: when a tracked reference streams out, tell managed code
  // so per-form behaviours detach instead of ticking on a stale handle. Fires on
  // the main thread (cell streaming); QueueEvent is drained next frame.
  quest_world_.set_on_unregister([host](u64 form) {
    host->QueueEvent({rec::script::host::ManagedEventId::kFormUnloaded, form, 0, 0, 0.0f});
  });
  ctx_.managed = host;  // let subsystems (interaction, ...) raise managed events
}

bool Engine::LoadGameData() {
  game_ = config_.game != bethesda::Game::kUnknown
              ? config_.game
              : bethesda::GameProfile::DetectFromDataDir(config_.data_dir);
  if (game_ == bethesda::Game::kUnknown) {
    REC_ERROR("could not detect a supported game in {}", config_.data_dir);
    return false;
  }
  ctx_.game = game_;
  const auto& profile = bethesda::GameProfile::For(game_);
  REC_INFO("detected {}", profile.name);

  MountArchives();
  // Loose files mount last so they win over archives.
  vfs_.Mount(asset::MakeLooseFileProvider(config_.data_dir));

  assets_ = std::make_unique<asset::AssetDatabase>(vfs_);
  ctx_.assets = assets_.get();
  bethesda::RegisterConverters(*assets_, profile);

  auto order = bethesda::LoadOrder::FromPluginsTxt(config_.plugins_txt, profile);
  if (!records_.LoadAll(config_.data_dir, order, profile)) return false;
  REC_INFO("{} plugins, {} records", order.plugins().size(), records_.record_count());

  // Localized string tables, base masters first so their ids win the collisions
  // a single id-keyed table cannot avoid (the main quest text lives in the base
  // game master). Plugins without string files (non-localized) are skipped.
  for (const std::string& plugin : order.plugins())
    strings_.Load(vfs_, plugin, profile.string_language);
  REC_INFO("loaded {} localized strings", strings_.size());

  // Index dialogue topics by quest so an NPC conversation opens without
  // rescanning every DIAL.
  dialogue_.Build(records_);
  REC_INFO("dialogue: {} topics indexed", dialogue_.topic_count());

  // The Papyrus guest: a separate, single-threaded world that runs game scripts
  // off the main thread. Form natives read the RecordStore; actor values and
  // inventory are backed by the bindings' own stores.
  script_bindings_ = std::make_unique<rec::script::skyrim::RecordBackedSkyrimBindings>(&records_);
  ctx_.bindings = script_bindings_.get();
  script_bindings_->set_strings(&strings_);
  script_bindings_->set_player(rec::script::papyrus::ObjectRef{0x14});  // Skyrim player ref
  // Route quest-driven world mutations (PlaceAtMe/MoveTo/Enable/Delete + cleanup)
  // through the provenance layer; the player teleports through a host hook since
  // it is an actor/capsule, not a registry entity.
  script_bindings_->set_world_sink(&runtime_world_sink_);
  quest_world_.set_on_move_player([this](u64 dest_ref, f32 x, f32 y, f32 z) {
    // When a quest warps the player to a reference inside an interior cell (the
    // Helgen keep, say), stream that cell first so the player lands in a loaded
    // world rather than at interior-local coordinates floating in the exterior.
    if (dest_ref != 0 && streamer_) {
      const bethesda::GlobalFormId ref{static_cast<u16>(dest_ref >> 32),
                                       static_cast<u32>(dest_ref & 0xffffffffu)};
      const bethesda::GlobalFormId interior = records_.InteriorCellOfRef(ref);
      if (interior.plugin != 0xffff) {
        Vec3 spawn;
        if (streamer_->EnterInterior(world_, interior, &spawn))
          REC_INFO("quest: entered interior {:04x}:{:06x} to move the player", interior.plugin,
                   interior.local_id);
      } else if (streamer_->in_interior()) {
        streamer_->EnterExterior(world_);  // a move back out to the worldspace
      }
    }
    actors_->TeleportPlayer(x, y, z);
  });
  // A connecting client is a passive replica: the server runs the scripts and is
  // authoritative for quest and quest-driven world state; the client mirrors it
  // through replicated quest snapshots and world commands. Definitions still load
  // (for journal text), but the client's own scripts may not mutate that state.
  // The host and single-player stay authoritative.
  script_bindings_->set_replica_mode(!config_.connect_address.empty());
  if (script_bindings_->replica_mode())
    REC_INFO("multiplayer client: quests run server-authoritative (replica mode)");
  scripts_ = std::make_unique<rec::script::ScriptSystem>(game_, &vfs_, script_bindings_.get());
  ctx_.scripts = scripts_.get();
  // Hand the bindings the guest VM so quest stage fragments can execute (run on
  // the guest thread, where the bindings live).
  {
    auto* binds = script_bindings_.get();
    scripts_->guest().Submit(
        [binds](rec::script::papyrus::VirtualMachine& vm) { binds->set_vm(&vm); });
  }
  // Route Debug.Notification (from Papyrus quests or C# mods) to the HUD toast.
  // Set on the guest thread so it never races the native; the handler only queues,
  // and the main loop drains it to the UI.
  {
    auto* guest = &scripts_->guest();
    guest->Submit([guest, this](rec::script::papyrus::VirtualMachine&) {
      guest->set_on_notification([this](const std::string& message) {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        pending_notifications_.push_back(message);
      });
    });
  }
  quest_->AttachQuestScripts();

  // Load any additional games as live secondary domains before the managed world
  // boots, so C# mods see every game's content at once.
  LoadExtraDomains();

  // Bring up the managed (C#) scripting world over the same guest, so user mods
  // and Skyrim soft logic run alongside Papyrus. Optional and gracefully absent.
  BootManagedScripting();

  // REC_QUEST_REPORT=<EDID> drives a quest through its stages to completion and
  // prints the journey, then quits; REC_DIALOGUE_REPORT dumps its dialogue.
  if (const char* want = std::getenv("REC_QUEST_REPORT")) {
    quest_->ReportQuestToCompletion(want);
    quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_DIALOGUE_REPORT")) {
    quest_->ReportDialogue(want);
    quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_SCENE_REPORT")) {
    quest_->ReportSceneFragments(want);
    quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_SCENE_PLAY")) {
    quest_->ReportScenePlay(want);
    quit_.store(true, std::memory_order_relaxed);
  }
  if (const char* want = std::getenv("REC_SCENE_LIVE")) {
    quest_->ReportSceneLive(want);
    quit_.store(true, std::memory_order_relaxed);
  }
  // Headless reports drive quests during init and quit before the main loop, so
  // deliver any events they queued to managed hooks here too.
  if (managed_) managed_->DrainEvents();

  // Actor bringup scene: load a Skyrim character and animate it, no streaming.
  if (config_.demo_scene == "actor") return actors_->CreateSkyrimActor();

  streamer_ = std::make_unique<world::CellStreamer>(records_, *assets_);
  ctx_.streamer = streamer_.get();
  // Forward load-door cell transitions to the managed world (LocationChanged), so
  // mods react to where the player is. Runs on the main thread; drained next frame.
  if (managed_) {
    auto* host = managed_.get();
    streamer_->set_on_location_change([host](u64 cell, bool interior) {
      host->QueueEvent(
          {rec::script::host::ManagedEventId::kLocationChanged, cell, 0, interior ? 1 : 0, 0.0f});
    });
  }
  // Register streamed NPCs in the quest world so quests can target them and
  // clients can apply replicated actor transforms by form id.
  streamer_->set_quest_world(&quest_world_);
  if (physics_.initialized()) {
    streamer_->set_physics(&physics_);
    physics_.set_water_height([this](const Vec3& position, f32* height, Vec3* flow) {
      return streamer_->WaterHeightAt(position, height, flow);
    });
  }
  world::CellStreamer::Settings settings;
  settings.grass_density = config_.grass_density;
  streamer_->Configure(settings);
  if (!config_.headless) {
    world::CellStreamer::Uploads uploads;
    uploads.mesh = [this](const asset::Mesh& mesh) { return renderer_.UploadMesh(mesh); };
    uploads.texture = [this](const asset::Texture& texture) {
      return renderer_.UploadTexture(texture);
    };
    uploads.material = [this](const asset::Material& material) {
      return renderer_.UploadMaterial(material);
    };
    streamer_->SetUploads(std::move(uploads));
  }

  if (!config_.interior.empty()) return LoadInterior();
  if (!streamer_->SelectWorldspace(profile.exterior_worldspace)) return false;

  // Without an explicit --cell, start in the game's content-dense cell so the
  // first view is its signature locale, not whichever grid Whiterun happens to
  // share. (0,-3) is the busiest New Atlantis exterior cell.
  if (!config_.start_cell_explicit && game_ == bethesda::Game::kStarfield) {
    config_.start_cell_x = 0;
    config_.start_cell_y = -3;
  }

  // Drop the camera a bit above the terrain at the middle of the start cell.
  constexpr f32 kUnitsToMeters = 0.01428f;
  constexpr f32 kCellSize = 4096.0f;
  f32 beth_x = (static_cast<f32>(config_.start_cell_x) + 0.5f) * kCellSize;
  f32 beth_y = (static_cast<f32>(config_.start_cell_y) + 0.5f) * kCellSize;
  Vec3 start{beth_x * kUnitsToMeters, 0.0f, -beth_y * kUnitsToMeters};
  f32 ground = 0;
  if (streamer_->GroundHeight(start.x, start.z, &ground)) {
    start.y = ground + 10.0f;  // a little above the terrain for a view
  } else {
    REC_WARN("no terrain at start cell {},{}", config_.start_cell_x, config_.start_cell_y);
  }
  camera_.set_position(start);
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 30.0f;
  REC_INFO("camera start: cell {},{} at ({:.1f}, {:.1f}, {:.1f})", config_.start_cell_x,
           config_.start_cell_y, start.x, start.y, start.z);
  actors_->MaybeSpawnWorldPlayer({start.x, ground, start.z});  // on the terrain, not 10m up
  showcase_regions_.push_back({{start.x, ground, start.z}, std::string(profile.name)});
  SetupExtraStreamers();

  // Tell the editor which games' assets it can place: the primary, then each
  // secondary content domain (so a Fallout 4 prop can be dropped into Skyrim).
  if (editor_) {
    std::vector<EditorPlaceDomain> domains;
    domains.push_back(
        {std::string(profile.name), GameSlug(game_), &records_, &strings_, streamer_.get()});
    for (size_t i = 0; i < extra_domains_.size() && i < extra_streamers_.size(); ++i) {
      ContentDomain& d = *extra_domains_[i];
      domains.push_back({std::string(d.profile().name), GameSlug(d.profile().game), &d.records(),
                         &d.strings(), extra_streamers_[i].get()});
    }
    editor_->SetPlaceDomains(std::move(domains));
  }
  return true;
}

// A stable per-game slug stored in the editor's layout file, so a saved
// placement reloads against the same game next run.
static std::string GameSlug(bethesda::Game game) {
  switch (game) {
    case bethesda::Game::kSkyrimSe:
      return "skyrimse";
    case bethesda::Game::kFallout4:
      return "fallout4";
    case bethesda::Game::kFallout76:
      return "fallout76";
    case bethesda::Game::kStarfield:
      return "starfield";
    default:
      return "game";
  }
}

void Engine::SetupExtraStreamers() {
  if (config_.headless || extra_domains_.empty()) return;
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
  const Vec3 cam = camera_.position();

  for (size_t i = 0; i < extra_domains_.size(); ++i) {
    ContentDomain& domain = *extra_domains_[i];
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
    settings.grass_density = config_.grass_density;
    streamer->Configure(settings);
    world::CellStreamer::Uploads uploads;
    uploads.mesh = [this, salt](const asset::Mesh& mesh) {
      return renderer_.UploadMesh(mesh, salt);
    };
    uploads.texture = [this, salt](const asset::Texture& texture) {
      return renderer_.UploadTexture(texture, salt);
    };
    uploads.material = [this, salt](const asset::Material& material) {
      return renderer_.UploadMaterial(material, salt);
    };
    streamer->SetUploads(std::move(uploads));
    if (streamer->SelectWorldspace(domain.profile().exterior_worldspace)) {
      REC_INFO("secondary worldspace rendering: {} cell {},{} placed at ({:.0f}, {:.0f}, {:.0f})",
               domain.profile().name, region_x, region_y, place.x, place.y, place.z);
      // The showcase flies over each rendered region; its ground baseline sits the
      // same 10m below the placed camera-height anchor as the primary's does.
      showcase_regions_.push_back(
          {{place.x, place.y - 10.0f, place.z}, std::string(domain.profile().name)});
    } else {
      REC_WARN("secondary domain {} has no worldspace '{}': not rendered, assets still placeable",
               domain.profile().name, domain.profile().exterior_worldspace);
    }
    // Kept parallel to extra_domains_ even when not rendered, so the editor can
    // place this game's assets (PlaceObject needs the per-domain streamer/salt).
    extra_streamers_.push_back(std::move(streamer));
  }
}

bool Engine::LoadInterior() {
  bethesda::GlobalFormId cell_id;
  if (config_.interior.starts_with("0x") || config_.interior.starts_with("0X")) {
    // Load order form id: top byte is the plugin index for full plugins.
    u32 raw = static_cast<u32>(std::stoul(config_.interior.substr(2), nullptr, 16));
    cell_id = {static_cast<u16>(raw >> 24), raw & 0xffffff};
  } else {
    cell_id = records_.FindInteriorCell(config_.interior);
  }
  if (cell_id.plugin == 0xffff) {
    REC_ERROR("interior cell not found: {}", config_.interior);
    return false;
  }

  Vec3 start{};
  if (!streamer_->LoadInterior(world_, cell_id, &start)) return false;
  camera_.set_position(start);
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 5.0f;
  REC_INFO("camera start: interior {} at ({:.1f}, {:.1f}, {:.1f})", config_.interior, start.x,
           start.y, start.z);
  REC_INFO("interior {}: {} npcs loaded", config_.interior, streamer_->spawned_npc_count());
  actors_->MaybeSpawnWorldPlayer(start);
  return true;
}

void Engine::LoadExtraDomains() {
  for (const ExtraDomainConfig& cfg : config_.extra_domains) {
    auto domain = std::make_unique<ContentDomain>();
    // A multiplayer client mirrors the host, so secondary domains are replicas
    // there too: their scripts read content but do not drive authoritative state.
    if (!domain->Load(cfg.game, cfg.data_dir, cfg.plugins_txt,
                      /*replica_mode=*/!config_.connect_address.empty())) {
      REC_WARN("secondary domain failed to load: {}", cfg.data_dir);
      continue;
    }
    // Surface its Debug.Notification on the same HUD toast as the primary game.
    auto* guest = &domain->scripts()->guest();
    guest->Submit([guest, this](rec::script::papyrus::VirtualMachine&) {
      guest->set_on_notification([this](const std::string& message) {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        pending_notifications_.push_back(message);
      });
    });
    // Run that game's quests inside its own microvm (capped like the primary).
    domain->AttachQuestScripts(config_.max_quest_scripts);
    REC_INFO("secondary domain live: {} ({} records, isolated microvm)", domain->profile().name,
             domain->records().record_count());
    extra_domains_.push_back(std::move(domain));
  }
}

void Engine::MountArchives() {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(config_.data_dir, ec)) {
    std::string path = entry.path().string();
    // TODO: archive order should follow plugin order plus the ini resource
    // lists, alphabetical is a placeholder.
    if (auto provider = bethesda::OpenArchive(path)) vfs_.Mount(std::move(provider));
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

void Engine::UpdateCamera(f32 frame_delta) {
  if (!window_) return;
  const InputState& input = window_->input();

  // The pause menu freezes the camera and frees the cursor so it can click.
  bool menu = game_ui_.menu_open();
  bool kb = debug_ui_.wants_keyboard();
  // Modal overlays that consume Esc; captured before the branches below so the
  // Esc that closes one does not also open the pause menu this frame.
  bool modal = interaction_->dialogue_open() || interaction_->container_open();
  // The map editor takes over navigation and input while on; its own Esc cancels
  // (clear brush / cancel move), so the pause menu yields to it.
  const bool editor_on = editor_ && editor_->active();

  if (input.key_pressed(Key::kT) && !menu && !kb && !modal && !editor_on && actors_->HasPlayer()) {
    ctx_.walk_mode = !ctx_.walk_mode;
    REC_INFO("walk mode {}",
             ctx_.walk_mode ? "on (WASD move, Shift run, Space jump, C view)" : "off");
  }
  if (input.key_pressed(Key::kC) && !menu && !kb && !modal && !editor_on)
    ctx_.third_person = !ctx_.third_person;
  if (input.key_pressed(Key::kJ) && !menu && !kb && !modal && !editor_on) quest_->ToggleJournal();

  if (editor_on) {
    // Free-fly builder camera: right mouse looks, WASD/QE move (unless the search
    // box has the keyboard). The editor handles picking, placing and editing.
    const bool allow = !menu && !kb;
    const bool typing = editor_->wants_keyboard();
    camera_.Update(input, allow, allow && !typing, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    editor_->Update(input, frame_delta, allow);
  } else if (interaction_->container_open()) {
    interaction_->UpdateContainerInput(input);  // Esc closes the loot view
    interaction_->UpdateInteraction(false);     // freeze movement/activation while looting
  } else if (interaction_->dialogue_open()) {
    interaction_->UpdateDialogueInput(input);  // 1-4 select a topic, Esc to leave
    interaction_->UpdateInteraction(false);    // freeze movement/activation while talking
  } else if (quest_->journal_open()) {
    // The journal is a modal overlay: a number key pins that quest to track;
    // movement is frozen while it is open.
    const Key num[4] = {Key::k1, Key::k2, Key::k3, Key::k4};
    for (int i = 0; i < 4; ++i)
      if (input.key_pressed(num[i])) quest_->PinJournalSlot(i);
    interaction_->UpdateInteraction(false);
  } else if (ctx_.walk_mode && actors_->HasPlayer()) {
    WalkUpdate(frame_delta, !menu && !kb);
    interaction_->UpdateInteraction(input.key_pressed(Key::kE) && !menu && !kb);
  } else {
    bool allow_mouse = !menu && (!debug_ui_.wants_mouse() || camera_.looking());
    bool allow_keyboard = !menu && !kb;
    camera_.Update(input, allow_mouse, allow_keyboard, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    interaction_->UpdateInteraction(false);  // clears any stale prompt outside walk mode
  }

  interaction_->SyncHud();   // mirror the conversation / loot view into the HUD
  DriveCamera(frame_delta);  // orbit / replay overrides + record

  if (input.key_pressed(Key::kF1) && !kb) debug_ui_.ToggleVisible();
  if (input.key_pressed(Key::kF2) && !kb) debug_ui_.ToggleTrace();
  if (input.key_pressed(Key::kF3) && !kb) debug_ui_.ToggleQuests();
  if (input.key_pressed(Key::kF4) && !kb && editor_) editor_->Toggle();
  if (input.key_pressed(Key::kF) && !menu && !kb && !ctx_.walk_mode && !editor_on)
    ThrowPhysicsCube();
  // The editor owns Esc (cancel brush / move); only open the pause menu outside it.
  if (input.key_pressed(Key::kEscape) && !kb && !modal && !editor_on) game_ui_.ToggleMenu();
  if (game_ui_.quit_requested()) RequestQuit();
}

void Engine::LookCameraAt(const Vec3& eye, const Vec3& center) {
  camera_.set_position(eye);
  Vec3 d = Normalize(center - eye);
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z),
                        std::asin(std::clamp(d.y, -1.0f, 1.0f)));  // forward() convention
}

void Engine::DriveCamera(f32 dt) {
  if (!cam_init_) {
    cam_init_ = true;
    // REC_CAM="x,y,z,yaw,pitch" pins the camera for a framed capture (handy for
    // screenshots that must show a specific vantage, e.g. two worlds side by side).
    if (const char* c = std::getenv("REC_CAM")) {
      Vec3 p{};
      f32 yaw = 0, pitch = 0;
      if (std::sscanf(c, "%f,%f,%f,%f,%f", &p.x, &p.y, &p.z, &yaw, &pitch) >= 3) {
        camera_.set_position(p);
        camera_.set_yaw_pitch(yaw, pitch);
      }
    }
    cam_orbit_ = std::getenv("REC_ORBIT") != nullptr;
    // REC_EDITOR boots straight into the map editor (the catalog is ready once
    // the records are loaded), so a capture or a builder session can skip F4.
    if (std::getenv("REC_EDITOR") && editor_ && !editor_->active()) editor_->Toggle();
    if (const char* r = std::getenv("REC_RECORD")) cam_record_ = std::fopen(r, "wb");
    if (const char* p = std::getenv("REC_REPLAY")) {
      if (std::FILE* f = std::fopen(p, "rb")) {
        f32 rec[7];
        while (std::fread(rec, sizeof(f32), 7, f) == 7) {
          cam_replay_.push_back({rec[0], {rec[1], rec[2], rec[3]}, {rec[4], rec[5], rec[6]}});
        }
        std::fclose(f);
        REC_INFO("camera replay: {} keys from {}", cam_replay_.size(), p);
      }
    }
    // REC_SHOWCASE flies a smooth cinematic pass over every loaded worldspace in
    // one take. REC_SHOWCASE_SHOTS=<dir> writes a regression PNG at each marked
    // beat; REC_SHOWCASE_QUIT exits when the pass ends (headless benchmark).
    if (std::getenv("REC_SHOWCASE")) {
      BuildShowcase();
      cam_showcase_ = !showcase_.empty();
      if (const char* d = std::getenv("REC_SHOWCASE_SHOTS")) showcase_shot_dir_ = d;
      showcase_quit_ = std::getenv("REC_SHOWCASE_QUIT") != nullptr;
      if (cam_showcase_) {
        ctx_.walk_mode = false;         // the cinematic owns the camera
        game_ui_.SetHudVisible(false);  // clean frames: no crosshair / compass / bars
        REC_INFO("showcase: {} waypoints over {} region(s), {:.1f}s{}", showcase_.size(),
                 showcase_regions_.size(), showcase_.duration(),
                 showcase_shot_dir_.empty() ? "" : " (capturing)");
      } else {
        REC_WARN("REC_SHOWCASE set but no worldspaces to fly over");
      }
    }
  }
  cam_time_ += dt;

  if (cam_showcase_) {
    f32 prev = cam_time_ - dt;
    ShowcasePose p = showcase_.Sample(cam_time_);
    LookCameraAt(p.eye, p.target);
    if (!showcase_shot_dir_.empty()) {
      std::string label;
      int idx = showcase_.CaptureCrossed(prev, cam_time_, &label);
      if (idx >= 0) {
        for (char& ch : label) {
          bool ok =
              (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
          if (!ok) ch = '_';
        }
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%02d_%s.png", showcase_shot_dir_.c_str(), idx,
                      label.c_str());
        renderer_.CaptureScreenshot(path);
        REC_INFO("showcase capture: {}", path);
      }
    }
    if (!showcase_done_) {
      // Benchmark steady-state render: skip the first second of warmup and any
      // half-second-plus frame (cold streaming/IO hitches, not GPU time). Real
      // mid-flight stutter (down to ~2 fps) is still counted, so it shows up.
      if (dt > 0 && dt < 0.5f && cam_time_ >= 1.0f) {
        showcase_dt_min_ = std::min(showcase_dt_min_, dt);
        showcase_dt_max_ = std::max(showcase_dt_max_, dt);
        showcase_bench_time_ += dt;
        ++showcase_frames_;
      }
      if (cam_time_ >= showcase_.duration()) {
        showcase_done_ = true;
        f32 avg =
            showcase_frames_ > 0 ? showcase_bench_time_ / static_cast<f32>(showcase_frames_) : 0.0f;
        REC_INFO("showcase done: {} frames over {:.1f}s, avg {:.0f} fps (min {:.0f}, max {:.0f})",
                 showcase_frames_, showcase_bench_time_, avg > 0 ? 1.0f / avg : 0.0f,
                 showcase_dt_max_ > 0 ? 1.0f / showcase_dt_max_ : 0.0f,
                 showcase_dt_min_ > 0 ? 1.0f / showcase_dt_min_ : 0.0f);
        if (showcase_quit_) RequestQuit();
      }
    }
  } else if (cam_orbit_) {
    f32 a = cam_time_ * 0.4f;  // radians/sec
    Vec3 center{0.0f, 1.0f, 0.0f};
    LookCameraAt({center.x + std::cos(a) * 6.0f, 2.4f, center.z + std::sin(a) * 6.0f}, center);
  } else if (!cam_replay_.empty()) {
    // Linear interpolation between the bracketing keys for the current time.
    const CamKey* lo = &cam_replay_[0];
    const CamKey* hi = lo;
    for (const CamKey& k : cam_replay_) {
      if (k.t <= cam_time_) lo = &k;
      if (k.t >= cam_time_) {
        hi = &k;
        break;
      }
    }
    f32 span = hi->t - lo->t;
    f32 u = span > 1e-5f ? std::clamp((cam_time_ - lo->t) / span, 0.0f, 1.0f) : 0.0f;
    auto mix = [&](const Vec3& a, const Vec3& b) { return a + (b - a) * u; };
    LookCameraAt(mix(lo->pos, hi->pos), mix(lo->target, hi->target));
  }

  if (cam_record_) {
    Vec3 p = camera_.position(), t = camera_.target();
    f32 rec[7] = {cam_time_, p.x, p.y, p.z, t.x, t.y, t.z};
    std::fwrite(rec, sizeof(f32), 7, cam_record_);
    std::fflush(cam_record_);  // survive a timeout kill
  }
}

void Engine::BuildShowcase() {
  // Per-worldspace drone beats, as offsets in metres from the region centre C
  // (ground level). Regions are placed along +X (east), so each pass enters from
  // the west and exits east, which keeps the glide between worldspaces short.
  auto wp = [](Vec3 eye, Vec3 look, f32 travel, bool capture, std::string label) {
    return ShowcaseCamera::Waypoint{eye, look, travel, capture, std::move(label)};
  };
  // Lead-in: hold on the opening vantage so the primary world (which streams
  // around the camera) has time to populate before the move and the first
  // capture. Secondary worldspaces stream around their own fixed anchor, so they
  // are already loaded by the time the camera reaches them.
  if (!showcase_regions_.empty()) {
    const Vec3 c0 = showcase_regions_[0].center;
    showcase_.Add(wp(c0 + Vec3{-180, 110, 80}, c0 + Vec3{-20, 25, 0}, 0.0f, false, {}));
  }
  // Every waypoint sits at ~zero velocity (smoothstep), so each is a clean,
  // well-framed still: capture them all for broad regression coverage and so a
  // good YouTube frame can be picked from the set. Files sort by play order.
  for (size_t k = 0; k < showcase_regions_.size(); ++k) {
    const Vec3 c = showcase_regions_[k].center;
    const std::string& n = showcase_regions_[k].name;
    if (k > 0) {
      // High traveling glide over the seam: both worldspaces are in frame here,
      // which makes it both a strong showcase beat and a regression mark.
      Vec3 mid = (showcase_regions_[k - 1].center + c) * 0.5f;
      showcase_.Add(wp(mid + Vec3{0, 170, 70}, mid + Vec3{70, 0, -20}, 9.0f, true, "seam"));
    }
    // Establishing: high to the west, looking down into the region. For region 0
    // this is the same pose as the lead-in, so the camera holds there first.
    showcase_.Add(
        wp(c + Vec3{-180, 110, 80}, c + Vec3{-20, 25, 0}, k == 0 ? 5.0f : 7.0f, true, n + "_wide"));
    // Descending push-in toward the centre.
    showcase_.Add(wp(c + Vec3{-70, 38, -30}, c + Vec3{40, 15, -60}, 5.0f, true, n + "_descend"));
    // Low skim across the centre.
    showcase_.Add(wp(c + Vec3{-10, 22, 25}, c + Vec3{110, 10, -10}, 7.0f, true, n + "_skim"));
    // Crane up and east, revealing the vista and setting up the exit.
    showcase_.Add(wp(c + Vec3{120, 92, 70}, c + Vec3{30, 15, 0}, 5.0f, true, n + "_reveal"));
  }
}

void Engine::WalkUpdate(f32 dt, bool allow) {
  const InputState& input = window_->input();
  window_->SetRelativeMouseMode(true);  // FPS-style mouse look in walk mode

  if (allow) {
    ctx_.cam_yaw += input.mouse_dx * camera_.sensitivity;
    cam_pitch_ = std::clamp(cam_pitch_ - input.mouse_dy * camera_.sensitivity, -1.4f, 1.4f);
  }

  // Move relative to where the camera faces (flattened to the ground plane).
  Vec3 fwd{std::sin(ctx_.cam_yaw), 0, -std::cos(ctx_.cam_yaw)};
  Vec3 right{std::cos(ctx_.cam_yaw), 0, std::sin(ctx_.cam_yaw)};
  Vec3 move{};
  if (allow) {
    if (input.key(Key::kW)) move = move + fwd;
    if (input.key(Key::kS)) move = move - fwd;
    if (input.key(Key::kD)) move = move + right;
    if (input.key(Key::kA)) move = move - right;
  }
  f32 speed = (allow && input.key(Key::kLeftShift)) ? 4.8f : 1.8f;
  if (ctx_.auto_walk) {
    // Test hook: head for the active quest marker / guide mark when one is set,
    // so the guided playthrough follows the quest; otherwise coast forward.
    // Route through pathfinding so the player rounds interior walls (the keep)
    // toward the goal rather than pressing straight into them.
    Vec3 ppos;
    if (ctx_.auto_walk_has_goal && actors_->PlayerWorldPos(&ppos)) {
      const Vec3 wp = npc_->PathToward(ppos, ctx_.auto_walk_goal);
      Vec3 to{wp.x - ppos.x, 0, wp.z - ppos.z};
      const f32 len = Length(to);
      move = len > 0.5f ? to * (1.0f / len) : fwd;
    } else {
      move = fwd;
    }
  }
  f32 move_len = Length(move);
  Vec3 velocity{};
  f32 yaw = 0;
  const bool moving = move_len > 0.01f;
  if (moving) {
    move = move * (1.0f / move_len);
    velocity = move * speed;
    yaw = std::atan2(move.x, move.z);  // the biped's +Z faces movement
  }
  bool jump = allow && input.key_pressed(Key::kSpace);

  // The actor system owns the player capsule; it steps the character controller
  // and returns the body (feet) position for the follow camera.
  Vec3 body{};
  actors_->MovePlayer(velocity, jump, yaw, moving, speed, dt, &body);

  Vec3 cam_fwd{std::cos(cam_pitch_) * std::sin(ctx_.cam_yaw), std::sin(cam_pitch_),
               -std::cos(cam_pitch_) * std::cos(ctx_.cam_yaw)};
  if (ctx_.third_person) {
    Vec3 pivot = body + Vec3{0, 1.5f, 0};
    ctx_.walk_eye = pivot - cam_fwd * 3.2f;
    ctx_.walk_target = pivot;
  } else {
    ctx_.walk_eye = body + Vec3{0, 1.7f, 0};
    ctx_.walk_target = ctx_.walk_eye + cam_fwd;
  }
}

void Engine::ThrowPhysicsCube() {
  if (!physics_.initialized() || !physics_cube_mesh_) return;
  Vec3 forward = camera_.forward();
  Vec3 origin = camera_.position() + forward * 0.8f;
  // Wood-ish density: heavy enough to splash, light enough to float.
  physics::BodyId body =
      physics_.AddDynamicBox(origin, {0.25f, 0.25f, 0.25f}, 350.0f, forward * 14.0f);
  if (!body) return;
  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {origin.x, origin.y, origin.z}});
  world_.Add(entity, world::Renderable{physics_cube_mesh_});
  physics_entities_.push_back({body, entity});
}

}  // namespace rec
