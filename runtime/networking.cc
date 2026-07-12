#include "engine.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <base/option.h>

#include "asset/primitives.h"
#include "core/log.h"
#include "quest/quest_def.h"
#include "script/papyrus/value.h"

#if RECREATION_HAS_NET
#include "modstream/content_provider.h"
#include "modstream/content_store.h"
#include "modstream/mod_catalog.h"
#include "gamenet/asset_stream.h"
#endif

// Engine network bringup: opens the authoritative server or replica client
// session and wires the replication sinks (quest journal, quest-driven world
// commands, NPC actor streaming, dialogue/stage requests) between the net layer
// and the script/quest systems. Built as a separate Engine translation unit;
// the whole file compiles away when RECREATION_HAS_NET is off.
namespace rx {

#if RECREATION_HAS_NET
static base::Option<bool> NetQuestLog{"net.quest.log", false, "RX_NET_QUEST_LOG"};
// Streaming-bubble radius in world units. Every joining player gets an
// interest bubble of this size and the server streams each client only the
// replicated entities inside it (per-peer delta streams instead of a
// broadcast) -- the bandwidth lever that scales the player count. 0 restores
// full-visibility broadcasting.
static base::Option<int> NetBubbleRadius{"net.bubble.radius", 128, "REC_NET_BUBBLE_RADIUS"};

bool StartNetworking(Engine& engine) {
  Engine* const self = &engine;
  net::GameSessionConfig net_config;
  net_config.port = self->config_.port;
  net_config.player_name = base::NameString(self->config_.player_name.c_str());
  net_config.max_clients = self->config_.max_clients;
  // Joining players replicate as cubes until there are real actor assets.
  net_config.player_mesh = asset::MakeAssetId("builtin/cube").hash;
  net_config.bubble_radius = static_cast<f32>(NetBubbleRadius.get());

  // Asset streaming endpoints: the host catalogs its mods directory to offer, a
  // connecting client opens the content cache it streams into. A configured but
  // unreadable mods directory is a hard error, not a silent skip.
  if (self->config_.host_server) {
    if (!self->config_.mods_dir.empty()) {
      std::optional<modstream::ModCatalog> catalog =
          modstream::ModCatalog::Build(self->config_.mods_dir);
      if (!catalog) {
        RX_ERROR("net: could not catalog mods directory '{}'", self->config_.mods_dir);
        return false;
      }
      RX_INFO("net: offering {} mod files ({} bytes) from {}",
               catalog->manifest().TotalFiles(), catalog->manifest().TotalBytes(),
               self->config_.mods_dir);
      self->mod_catalog_ = std::make_unique<modstream::ModCatalog>(std::move(*catalog));
      net_config.mod_catalog = self->mod_catalog_.get();
      // Mount the host's own mods so a listen server (and headless physics/nav)
      // sees exactly what it streams to clients. Mounted after the base game, so
      // mods win, just like loose files.
      modstream::MountCatalog(*self->vfs_, *self->mod_catalog_);
    }
  } else if (!self->config_.connect_address.empty()) {
    const std::string cache_dir = self->config_.asset_cache_dir.empty()
                                      ? std::string("recreation_asset_cache")
                                      : self->config_.asset_cache_dir;
    self->content_store_ = std::make_unique<modstream::ContentStore>(cache_dir);
    net_config.content_store = self->content_store_.get();
  }

  if (self->config_.host_server) {
    auto server = std::make_unique<net::GameServerSession>(std::move(net_config));
    if (!server->Start()) return false;
    self->server_session_ = server.get();
    self->ctx_.server_session = self->server_session_;
    self->session_ = std::move(server);
    // Replicate the authoritative quest journal. The source is only called when
    // clients are connected, so the guest round-trip costs nothing while idle.
    // Quest state lives on the guest thread, so we marshal the read onto it.
    if (self->scripts_ && self->script_bindings_) {
      self->server_session_->SetQuestSource([self]() -> std::vector<net::DomainQuestStatus> {
        // Replicate every loaded game's journal, each tagged with its domain so
        // the client routes it to the matching game.
        std::vector<net::DomainQuestStatus> all;
        auto collect = [&](u8 domain, rx::script::ScriptSystem* scripts,
                           rx::script::skyrim::RecordBackedSkyrimBindings* binds) {
          if (!scripts || !binds) return;
          auto statuses = scripts->guest()
                              .SubmitFor([binds](script::papyrus::VirtualMachine&) {
                                return binds->quest_system().AllStatuses();
                              })
                              .get();
          for (quest::QuestStatus& s : statuses) all.push_back({domain, std::move(s)});
        };
        collect(0, self->scripts_.get(), self->script_bindings_.get());
        for (size_t i = 0; i < self->extra_domains_.size(); ++i) {
          collect(static_cast<u8>(i + 1), self->extra_domains_[i]->scripts(),
                  self->extra_domains_[i]->bindings());
        }
        return all;
      });
      // The Civil War campaign board is C# state on the host only, so replicate
      // it: the session ships it to clients whenever it changes (or one joins).
      self->server_session_->SetWarMapSource([self]() -> net::WarMapState {
        net::WarMapState board;
        std::vector<script::skyrim::RecordBackedSkyrimBindings::WarHold> holds;
        f32 fraction = 0.0f;
        self->script_bindings_->SnapshotWarMap(holds, fraction);
        board.imperial_fraction = fraction;
        board.holds.reserve(holds.size());
        for (const auto& h : holds) board.holds.push_back({h.name, h.owner});
        return board;
      });
      // A client activating a reference runs OnActivate authoritatively here; the
      // resulting quest/world changes replicate back through the usual channels.
      self->server_session_->SetActivateSink([self](u64 handle) { self->interaction_->RaiseActivate(handle); });
      // A client picking a dialogue topic runs that INFO's fragment here, so the
      // quest advances on the server and replicates to everyone.
      self->server_session_->SetDialogueSink([self](u64 info) { self->interaction_->RunInfoFragment(info); });
      // A client's quest debugger acts through the server: apply the requested
      // stage/objective/running change on the guest, which replicates back as a
      // normal quest update.
      self->server_session_->SetStageRequestSink([self](const net::StageRequest& r) {
        if (!self->scripts_) return;
        auto* binds = self->script_bindings_.get();
        self->scripts_->guest().Submit([binds, r](script::papyrus::VirtualMachine&) {
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
    self->server_session_->SetActorSource([self]() { return net::CollectActorStates(*self->world_); });
    // When a client finishes streaming the mods, raise a managed event so
    // server-side C# scripts can react (gate spawn, greet the player).
    self->server_session_->SetClientReadySink([self](u32 peer) {
      RX_INFO("net: peer {} finished streaming the server's mods", peer);
      if (self->managed_)
        self->managed_->QueueEvent({rx::script::host::ManagedEventId::kClientAssetsReady,
                                    peer, 0, 0, 0.0f});
    });
    // The fundamental multiplayer hooks: a player joined, a player left. Raised
    // for every peer so server-side scripts work even without streamed mods.
    self->server_session_->SetClientJoinedSink([self](u32 peer) {
      if (self->managed_)
        self->managed_->QueueEvent({rx::script::host::ManagedEventId::kClientJoined,
                                    peer, 0, 0, 0.0f});
    });
    self->server_session_->SetClientLeftSink([self](u32 peer) {
      if (self->managed_)
        self->managed_->QueueEvent({rx::script::host::ManagedEventId::kClientLeft,
                                    peer, 0, 0, 0.0f});
    });
  } else if (!self->config_.connect_address.empty()) {
    net_config.address = base::String(self->config_.connect_address.c_str());
    auto client = std::make_unique<net::GameClientSession>(std::move(net_config));
    if (!client->Start()) return false;
    self->client_session_ = client.get();
    self->ctx_.client_session = self->client_session_;
    self->session_ = std::move(client);
    // Mount the streamed mods into the asset Vfs once the whole manifest has
    // landed in the cache, so the host's custom content resolves like loose files.
    if (self->content_store_ && self->client_session_->asset_stream()) {
      self->client_session_->asset_stream()->set_on_ready(
          [self](const modstream::ModManifest& manifest) {
            // Replace any previous mount (a live reload re-fires this), on the main
            // thread where nothing is reading the Vfs.
            self->vfs_->UnmountByPrefix("modstream:");
            modstream::MountManifest(*self->vfs_, manifest, *self->content_store_);
            RX_INFO("net: mounted {} streamed mod files into the asset vfs",
                     manifest.TotalFiles());
          });
    }
    // Mirror the server's journal onto our quest system. ApplyStatus mutates
    // quest state, so it has to run on the guest thread like every other write.
    if (self->scripts_ && self->script_bindings_) {
      self->client_session_->SetQuestSink([self](u8 domain, const quest::QuestStatus& status) {
        // Route the replicated quest to the game it belongs to: 0 is the primary
        // game, 1..N the secondary domains loaded in the same order as the host.
        rx::script::ScriptSystem* scripts = nullptr;
        rx::script::skyrim::RecordBackedSkyrimBindings* binds = nullptr;
        if (domain == 0) {
          scripts = self->scripts_.get();
          binds = self->script_bindings_.get();
        } else if (static_cast<size_t>(domain - 1) < self->extra_domains_.size()) {
          scripts = self->extra_domains_[domain - 1]->scripts();
          binds = self->extra_domains_[domain - 1]->bindings();
        }
        if (!scripts || !binds) return;
        scripts->guest().Submit([binds, status](script::papyrus::VirtualMachine&) {
          // Apply via the binding (not quest_system directly) so a replicated stage
          // advance also fires the managed QuestStageChanged event, driving the C#
          // questing gameplay (XP, journal) on the client as on the host.
          binds->ApplyReplicatedStatus(status);
        });
        if (NetQuestLog)
          RX_INFO("net: applied domain {} quest 0x{:x} stage {} complete {}", domain,
                   status.handle, status.stage, status.complete ? 1 : 0);
      });
      // Mirror the host's quest-driven world effects (spawns/moves/disables/
      // cleanup). Runs in the net sim stage on the main thread, which owns the
      // ECS, so applying straight to QuestWorld is safe.
      self->client_session_->SetWorldCommandSink(
          [self](const std::vector<world::WorldCommand>& cmds) { self->quest_world_->Apply(cmds); });
      // Mirror authoritative NPC movement onto our existing (cell-loaded) NPC
      // entities, interpolated between updates.
      self->client_session_->SetActorSink([self](const std::vector<net::ActorState>& actors) {
        net::ApplyActorStates(*self->world_, *self->quest_world_, actors, 0.1f);
      });
      // Show the host's active objective waypoint on our own compass: store its
      // world position; UpdateObjectiveMarkers turns it into a local bearing.
      self->client_session_->SetObjectiveMarkerSink([self](const net::ObjectiveMarkerState& m) {
        self->quest_->SetRemoteMarker(m.active, Vec3{m.x, m.y, m.z});
      });
      // Mirror the host's Civil War board onto our war-map panel (the campaign C#
      // runs only on the host). SetWarHold/SetWarProgress lock the binding's
      // war-map mutex, so applying from the net sim thread is safe.
      self->client_session_->SetWarMapSink([self](const net::WarMapState& board) {
        if (!self->script_bindings_) return;
        for (size_t i = 0; i < board.holds.size(); ++i)
          self->script_bindings_->SetWarHold(static_cast<i32>(i), board.holds[i].name,
                                             board.holds[i].owner);
        self->script_bindings_->SetWarProgress(board.imperial_fraction);
      });
    }
  } else {
    return true;
  }

  self->scheduler_->AddSystem(ecs::Stage::kSim, "net",
                       [self](ecs::World& world, f32 dt) { self->session_->Tick(world, dt); });
  if (self->client_session_) {
    // Remote transforms blend between snapshots. With a renderer that runs
    // per frame; headless clients smooth at the fixed step instead.
    const ecs::Stage stage = self->config_.headless ? ecs::Stage::kPostSim : ecs::Stage::kPreRender;
    self->scheduler_->AddSystem(stage, "net_interpolation",
                         [](ecs::World& world, f32 dt) { net::TickInterpolation(world, dt); });
  }
  // The managed world booted before the session, so forward any RPC names its
  // mods subscribed to into the live session's registry now.
  if (self->managed_) RegisterManagedRpcForwarding(*self);
  return true;
}

void ReloadMods(Engine& engine) {
  Engine* const self = &engine;
  if (self->config_.mods_dir.empty() || !self->server_session_ || !self->mod_catalog_) {
    return;  // not hosting a mods directory; nothing to reload
  }
  std::optional<modstream::ModCatalog> fresh =
      modstream::ModCatalog::Build(self->config_.mods_dir);
  if (!fresh) {
    RX_ERROR("net: mod reload failed to catalog '{}', keeping the current set",
              self->config_.mods_dir);
    return;  // a broken edit must not take down the running server
  }
  RX_INFO("net: reloaded mods, now offering {} files ({} bytes)",
           fresh->manifest().TotalFiles(), fresh->manifest().TotalBytes());

  // Point the session at the new catalog before the old one is destroyed, then
  // swap ownership: the new catalog object's address is stable across the move.
  auto next = std::make_unique<modstream::ModCatalog>(std::move(*fresh));
  self->server_session_->ReloadCatalog(*next);
  self->mod_catalog_ = std::move(next);

  // Re-mount on the host: drop the old mod providers and mount the new catalog,
  // on the main thread where nothing is reading the Vfs.
  self->vfs_->UnmountByPrefix("modstream:");
  modstream::MountCatalog(*self->vfs_, *self->mod_catalog_);
}
#endif  // RECREATION_HAS_NET

}  // namespace rx
