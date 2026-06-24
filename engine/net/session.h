#ifndef RECREATION_NET_SESSION_H_
#define RECREATION_NET_SESSION_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <znet/z_client.h>
#include <znet/z_server.h>

#include <functional>
#include <memory>

#include "ecs/world.h"
#include "net/actor_sync.h"
#include "net/objective_marker_net.h"
#include "net/protocol.h"
#include "net/quest_replication.h"
#include "net/replication.h"
#include "net/stage_request.h"
#include "quest/quest_system.h"
#include "world/quest_world.h"

namespace rec::modstream {
class ModCatalog;
class ContentStore;
}  // namespace rec::modstream

namespace rec::net {

class AssetStreamServer;
class AssetStreamClient;
class RpcServerChannel;
class RpcClientChannel;

struct SessionConfig {
  u16 port = 29700;
  base::String address;  // client: server to join
  base::NameString player_name{"player"};
  u32 max_clients = 64;
  u32 tick_rate = 60;
  u32 snapshot_interval_ticks = 3;   // 20 Hz at the 60 Hz fixed step
  u32 keyframe_interval_ticks = 60;  // full snapshot every second
  f32 client_timeout_seconds = 10.0f;
  u64 player_mesh = 0;  // AssetId hash spawned for joining players

  // Server: the catalogued mods directory to offer for streaming. Null leaves
  // asset streaming off (the session runs exactly as before).
  const modstream::ModCatalog* mod_catalog = nullptr;
  // Client: where streamed mod content is cached. Null leaves streaming off.
  modstream::ContentStore* content_store = nullptr;
};

// The server simulates, clients render what snapshots tell them. One process
// can run both (listen server) since sessions only touch the world through
// the ECS.
class Session {
 public:
  virtual ~Session() = default;

  // Called from the fixed-step sim stage.
  virtual void Tick(ecs::World& world, f32 dt) = 0;
};

class ServerSession final : public Session {
 public:
  explicit ServerSession(SessionConfig config);
  ~ServerSession() override;

  bool Start();
  void Tick(ecs::World& world, f32 dt) override;

  // Authoritative quest state to replicate, across every loaded game. Set by the
  // engine to collect each domain's QuestSystem::AllStatuses() tagged with its
  // domain id. When unset, no quest packets ship.
  void SetQuestSource(std::function<std::vector<DomainQuestStatus>()> source) {
    quest_source_ = std::move(source);
  }

  // Sink invoked for each kStageRequest a client sends. The engine wires this to
  // the authoritative QuestSystem so a client's debugger acts through the
  // server, whose change then replicates back as a normal QuestUpdate. When
  // unset, stage requests are decoded and dropped.
  void SetStageRequestSink(std::function<void(const StageRequest&)> sink) {
    stage_request_sink_ = std::move(sink);
  }

  // Replicates the active objective waypoint to every client on the reliable
  // channel. The engine calls this when the marker changes (active=false clears
  // the clients' pip), not every tick.
  void SendObjectiveMarker(const ObjectiveMarkerState& m);

  // Broadcasts a batch of quest-driven world commands (already drained and
  // applied locally by the host) to every client on the reliable channel. A
  // no-op when there are no clients or the list is empty.
  void SendWorldCommands(const std::vector<world::WorldCommand>& commands);

  // Sink invoked with the form handle each time a client activates a reference.
  // The engine wires this to raise OnActivate authoritatively, so quest/dialogue
  // logic runs on the server and replicates back. When unset, activations drop.
  void SetActivateSink(std::function<void(u64)> sink) { activate_sink_ = std::move(sink); }

  // Sink invoked with the INFO handle each time a client picks a dialogue topic.
  // The engine runs the INFO fragment authoritatively (advancing the quest).
  void SetDialogueSink(std::function<void(u64)> sink) { dialogue_sink_ = std::move(sink); }

  // Authoritative NPC transforms to stream. Set by the engine to walk the world's
  // NPC entities; only the ones that moved since last tick go out (unreliable).
  void SetActorSource(std::function<std::vector<ActorState>()> source) {
    actor_source_ = std::move(source);
  }

  // Sink invoked with the peer id when a client reports it finished streaming the
  // server's mods. The engine raises a managed event so server-side scripts can
  // react (gate spawn, greet the player). When unset, the notice is dropped.
  void SetClientReadySink(std::function<void(u32)> sink) {
    client_ready_sink_ = std::move(sink);
  }

  // Sinks invoked when a client joins and when it leaves (or times out), the
  // fundamental multiplayer hooks for server-side scripts. Fired for every peer
  // regardless of asset streaming. Unset sinks drop the notice.
  void SetClientJoinedSink(std::function<void(u32)> sink) {
    client_joined_sink_ = std::move(sink);
  }
  void SetClientLeftSink(std::function<void(u32)> sink) {
    client_left_sink_ = std::move(sink);
  }

  // The server's scripting RPC channel. Always present once Start succeeds; the
  // engine registers handlers and emits client-bound calls through it.
  RpcServerChannel* rpc() { return rpc_.get(); }

  // Swaps the mod catalog offered to clients (live reload). New joiners get the
  // new manifest; in-flight transfers, which captured their paths, are unaffected.
  // Call on the session thread.
  void ReloadCatalog(const modstream::ModCatalog& catalog);

  u32 client_count() const { return static_cast<u32>(clients_.size()); }
  u64 tick() const { return tick_; }

 private:
  struct RemoteClient {
    base::NameString name;
    ecs::Entity player = ecs::kInvalidEntity;
    u64 player_net_id = 0;
    PlayerInput input;
    f32 since_last_packet = 0;
  };

  void PollMessages(ecs::World& world);
  void HandleJoin(ecs::World& world, u32 peer, const ClientJoinView& join);
  void DropClient(ecs::World& world, u32 peer);
  void SimulatePlayers(ecs::World& world, f32 dt);
  void TimeoutClients(ecs::World& world, f32 dt);
  void BroadcastSnapshot(ecs::World& world);
  void BroadcastQuests();
  void BroadcastActors();

  SessionConfig config_;
  tx::network::ZServer server_;
  SnapshotBuilder builder_;
  Snapshot snapshot_;  // reused so the vectors keep their capacity
  base::UnorderedMap<u32, RemoteClient> clients_;
  base::Vector<u32> scratch_dropped_;
  std::function<std::vector<DomainQuestStatus>()> quest_source_;
  std::function<void(const StageRequest&)> stage_request_sink_;
  std::function<void(u64)> activate_sink_;
  std::function<void(u64)> dialogue_sink_;
  std::function<std::vector<ActorState>()> actor_source_;
  std::function<void(u32)> client_ready_sink_;
  std::function<void(u32)> client_joined_sink_;
  std::function<void(u32)> client_left_sink_;
  QuestReplicator quest_replicator_;
  ActorReplicator actor_replicator_;
  std::unique_ptr<AssetStreamServer> asset_stream_;
  std::unique_ptr<RpcServerChannel> rpc_;
  u64 tick_ = 0;
  bool force_keyframe_ = false;
  bool started_ = false;
};

class ClientSession final : public Session {
 public:
  explicit ClientSession(SessionConfig config);
  ~ClientSession() override;

  bool Start();
  void Tick(ecs::World& world, f32 dt) override;

  // Local input forwarded to the server every tick once joined.
  void SetInput(const PlayerInput& input) { input_ = input; }

  // Sends an activation request for `handle` to the server on the reliable
  // channel. The server is authoritative for the response (dialogue/quests).
  void SendActivate(u64 handle);
  // Sends the chosen dialogue INFO handle to the server, which runs its fragment.
  void SendDialogueSelect(u64 info);
  // Asks the server to apply a quest-debugger change. No-op until joined; the
  // result comes back through the normal quest replication.
  void SendStageRequest(const StageRequest& req);

  // Sink invoked once per quest in every kQuestUpdate received, with the quest's
  // domain id. The engine routes it to that game's QuestSystem::ApplyStatus so
  // each client journal mirrors the server. When unset, updates are dropped.
  void SetQuestSink(std::function<void(u8 domain, const quest::QuestStatus&)> sink) {
    quest_sink_ = std::move(sink);
  }

  // Sink invoked with the command list from every kWorldCommands received. The
  // engine wires this to QuestWorld::Apply so client world state (spawns, moves,
  // disables, cleanup) mirrors the host. When unset, updates are dropped.
  void SetWorldCommandSink(std::function<void(const std::vector<world::WorldCommand>&)> sink) {
    world_command_sink_ = std::move(sink);
  }

  // Sink invoked with the NPC transforms in each kActorSync received. The engine
  // wires this to ApplyActorStates so client NPCs mirror the host's movement.
  void SetActorSink(std::function<void(const std::vector<ActorState>&)> sink) {
    actor_sink_ = std::move(sink);
  }

  // Sink invoked once per kObjectiveMarker received. The engine wires this to
  // feed the HUD compass pip, recomputed from the client's own camera. When
  // unset, markers are decoded and dropped.
  void SetObjectiveMarkerSink(std::function<void(const ObjectiveMarkerState&)> sink) {
    objective_marker_sink_ = std::move(sink);
  }

  // The client's scripting RPC channel. Always present once Start succeeds; the
  // engine registers handlers and emits server-bound calls through it.
  RpcClientChannel* rpc() { return rpc_.get(); }

  // The asset-stream downloader, or null when streaming is off. The engine wires
  // its ready callback to mount the streamed mods into the asset Vfs.
  AssetStreamClient* asset_stream() { return asset_stream_.get(); }

  bool joined() const { return joined_; }
  u64 player_net_id() const { return player_net_id_; }
  ecs::Entity player_entity() const { return applier_.Find(player_net_id_); }
  u32 replicated_entity_count() const { return applier_.entity_count(); }

 private:
  void PollMessages(ecs::World& world);

  SessionConfig config_;
  tx::network::ZClient client_;
  SnapshotApplier applier_;
  std::unique_ptr<AssetStreamClient> asset_stream_;
  std::unique_ptr<RpcClientChannel> rpc_;
  std::function<void(u8 domain, const quest::QuestStatus&)> quest_sink_;
  std::function<void(const ObjectiveMarkerState&)> objective_marker_sink_;
  std::function<void(const std::vector<world::WorldCommand>&)> world_command_sink_;
  std::function<void(const std::vector<ActorState>&)> actor_sink_;
  PlayerInput input_;
  u64 player_net_id_ = 0;
  u64 tick_ = 0;
  f32 snapshot_dt_ = 1.0f / 20.0f;
  bool join_sent_ = false;
  bool joined_ = false;
  bool failure_logged_ = false;
};

}  // namespace rec::net

#endif  // RECREATION_NET_SESSION_H_
