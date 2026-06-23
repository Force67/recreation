#include "engine.h"

#include <cstdlib>
#include <utility>
#include <vector>

#include "asset/primitives.h"
#include "core/log.h"
#include "quest/quest_def.h"
#include "script/papyrus/value.h"

// Engine network bringup: opens the authoritative server or replica client
// session and wires the replication sinks (quest journal, quest-driven world
// commands, NPC actor streaming, dialogue/stage requests) between the net layer
// and the script/quest systems. Built as a separate Engine translation unit;
// the whole file compiles away when RECREATION_HAS_NET is off.
namespace rec {

#if RECREATION_HAS_NET
bool StartNetworking(Engine& engine) {
  Engine* const self = &engine;
  net::SessionConfig net_config;
  net_config.port = self->config_.port;
  net_config.player_name = base::NameString(self->config_.player_name.c_str());
  net_config.max_clients = self->config_.max_clients;
  // Joining players replicate as cubes until there are real actor assets.
  net_config.player_mesh = asset::MakeAssetId("builtin/cube").hash;

  if (self->config_.host_server) {
    auto server = std::make_unique<net::ServerSession>(std::move(net_config));
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
        collect(0, self->scripts_.get(), self->script_bindings_.get());
        for (size_t i = 0; i < self->extra_domains_.size(); ++i) {
          collect(static_cast<u8>(i + 1), self->extra_domains_[i]->scripts(),
                  self->extra_domains_[i]->bindings());
        }
        return all;
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
    self->server_session_->SetActorSource([self]() { return net::CollectActorStates(self->world_); });
  } else if (!self->config_.connect_address.empty()) {
    net_config.address = base::String(self->config_.connect_address.c_str());
    auto client = std::make_unique<net::ClientSession>(std::move(net_config));
    if (!client->Start()) return false;
    self->client_session_ = client.get();
    self->ctx_.client_session = self->client_session_;
    self->session_ = std::move(client);
    // Mirror the server's journal onto our quest system. ApplyStatus mutates
    // quest state, so it has to run on the guest thread like every other write.
    if (self->scripts_ && self->script_bindings_) {
      self->client_session_->SetQuestSink([self](u8 domain, const quest::QuestStatus& status) {
        // Route the replicated quest to the game it belongs to: 0 is the primary
        // game, 1..N the secondary domains loaded in the same order as the host.
        rec::script::ScriptSystem* scripts = nullptr;
        rec::script::skyrim::RecordBackedSkyrimBindings* binds = nullptr;
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
        if (std::getenv("REC_NET_QUEST_LOG"))
          REC_INFO("net: applied domain {} quest 0x{:x} stage {} complete {}", domain,
                   status.handle, status.stage, status.complete ? 1 : 0);
      });
      // Mirror the host's quest-driven world effects (spawns/moves/disables/
      // cleanup). Runs in the net sim stage on the main thread, which owns the
      // ECS, so applying straight to QuestWorld is safe.
      self->client_session_->SetWorldCommandSink(
          [self](const std::vector<world::WorldCommand>& cmds) { self->quest_world_.Apply(cmds); });
      // Mirror authoritative NPC movement onto our existing (cell-loaded) NPC
      // entities, interpolated between updates.
      self->client_session_->SetActorSink([self](const std::vector<net::ActorState>& actors) {
        net::ApplyActorStates(self->world_, self->quest_world_, actors, 0.1f);
      });
      // Show the host's active objective waypoint on our own compass: store its
      // world position; UpdateObjectiveMarkers turns it into a local bearing.
      self->client_session_->SetObjectiveMarkerSink([self](const net::ObjectiveMarkerState& m) {
        self->quest_->SetRemoteMarker(m.active, Vec3{m.x, m.y, m.z});
      });
    }
  } else {
    return true;
  }

  self->scheduler_.AddSystem(ecs::Stage::kSim, "net",
                       [self](ecs::World& world, f32 dt) { self->session_->Tick(world, dt); });
  if (self->client_session_) {
    // Remote transforms blend between snapshots. With a renderer that runs
    // per frame; headless clients smooth at the fixed step instead.
    const ecs::Stage stage = self->config_.headless ? ecs::Stage::kPostSim : ecs::Stage::kPreRender;
    self->scheduler_.AddSystem(stage, "net_interpolation",
                         [](ecs::World& world, f32 dt) { net::TickInterpolation(world, dt); });
  }
  return true;
}
#endif  // RECREATION_HAS_NET

}  // namespace rec
