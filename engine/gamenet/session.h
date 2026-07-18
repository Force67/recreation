#ifndef RECREATION_GAMENET_SESSION_H_
#define RECREATION_GAMENET_SESSION_H_

// Recreation's thin session layer over rx::net. The engine module owns the
// transport, the join handshake, per-peer entity streams and the streaming
// bubbles (interest + ownership); these wrappers add only what is game:
// quest/war-map/actor replication, dialogue/stage/activate routing, and the
// mod asset streaming. Everything crosses the wire as GameMessage payloads
// through the sessions' game-message seams.

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ecs/world.h"
#include "gamenet/actor_sync.h"
#include "gamenet/objective_marker_net.h"
#include "gamenet/protocol.h"
#include "gamenet/quest_replication.h"
#include "gamenet/stage_request.h"
#include "gamenet/war_map_net.h"
#include "net/session.h"
#include "quest/quest_system.h"
#include "world/quest_world.h"

namespace rx::modstream {
class ModCatalog;
class ContentStore;
}  // namespace rx::modstream

namespace rx::net {

class AssetStreamServer;
class AssetStreamClient;

struct GameSessionConfig {
  u16 port = 29700;
  base::String address;  // client: server to join
  base::NameString player_name{"player"};
  u32 max_clients = 64;
  u32 tick_rate = 60;
  u32 snapshot_interval_ticks = 3;   // 20 Hz at the 60 Hz fixed step
  u32 keyframe_interval_ticks = 60;  // full snapshot every second
  f32 client_timeout_seconds = 10.0f;
  u64 player_mesh = 0;  // AssetId hash spawned for joining players

  // Streaming-bubble radius in world units; every joining player gets a
  // bubble of this size and receives only the replicated entities inside it.
  // 0 replicates everything to everyone (the pre-bubble behavior).
  f32 bubble_radius = 0;

  // Server: the catalogued mods directory to offer for streaming. Null leaves
  // asset streaming off (the session runs exactly as before).
  const modstream::ModCatalog* mod_catalog = nullptr;
  // Client: where streamed mod content is cached. Null leaves streaming off.
  modstream::ContentStore* content_store = nullptr;
};

class GameServerSession final : public Session {
 public:
  static constexpr u32 kMaxActivationRequestsPerSecond = 32;

  explicit GameServerSession(GameSessionConfig config);
  ~GameServerSession() override;

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
  // server, whose change then replicates back as a normal QuestUpdate.
  void SetStageRequestSink(std::function<void(const StageRequest&)> sink) {
    stage_request_sink_ = std::move(sink);
  }

  // Replicates the active objective waypoint to every client on the reliable
  // channel. The engine calls this when the marker changes, not every tick.
  void SendObjectiveMarker(const ObjectiveMarkerState& m);

  // The Civil War campaign board to replicate (host-only C# state), shipped on
  // change or when a client joins.
  void SetWarMapSource(std::function<WarMapState()> source) { war_map_source_ = std::move(source); }

  // Broadcasts a batch of quest-driven world commands (already drained and
  // applied locally by the host) to every client on the reliable channel.
  void SendWorldCommands(const std::vector<world::WorldCommand>& commands);
  void SetWorldCommandSource(std::function<std::vector<world::WorldCommand>()> source) {
    world_command_source_ = std::move(source);
  }

  // Sink invoked with the admitted peer and form handle each time a client
  // activates a reference. The game validates that peer's authoritative player
  // against the target before running any script.
  void SetActivateSink(std::function<void(u32 peer, u64 handle)> sink) {
    activate_sink_ = std::move(sink);
  }

  // Sink invoked with the INFO handle each time a client picks a dialogue topic.
  void SetDialogueSink(std::function<void(u64)> sink) { dialogue_sink_ = std::move(sink); }

  // Authoritative NPC transforms to stream; only the ones that moved since the
  // last tick go out (unreliable).
  void SetActorSource(std::function<std::vector<ActorState>()> source) {
    actor_source_ = std::move(source);
  }

  // Sink invoked with the peer id when a client reports it finished streaming
  // the server's mods.
  void SetClientReadySink(std::function<void(u32)> sink) { client_ready_sink_ = std::move(sink); }

  // Join/leave hooks, forwarded from the engine session.
  void SetClientJoinedSink(std::function<void(u32)> sink);
  void SetClientLeftSink(std::function<void(u32)> sink) { client_left_sink_ = std::move(sink); }

  // The server's scripting RPC channel. Always present once Start succeeds.
  RpcServerChannel* rpc() { return inner_.rpc(); }

  // Swaps the mod catalog offered to clients (live reload).
  void ReloadCatalog(const modstream::ModCatalog& catalog);

  // The engine session underneath: interest map (bubbles/ownership), stats,
  // per-peer sends.
  ServerSession& engine() { return inner_; }
  const ServerSession& engine() const { return inner_; }

  u32 client_count() const { return inner_.client_count(); }
  u64 tick() const { return inner_.tick(); }

 private:
  void OnGameMessage(u32 peer, u16 type, const u8* data, size_t size);
  void BroadcastQuests();
  void BroadcastActors();
  void BroadcastWarMap();

  GameSessionConfig config_;
  ServerSession inner_;
  std::function<std::vector<DomainQuestStatus>()> quest_source_;
  std::function<std::vector<world::WorldCommand>()> world_command_source_;
  std::function<WarMapState()> war_map_source_;
  std::vector<u8> last_war_map_blob_;  // last board sent, to skip unchanged ticks
  size_t last_war_map_clients_ = 0;    // re-send the board when a new client joins
  std::function<void(const StageRequest&)> stage_request_sink_;
  std::function<void(u32, u64)> activate_sink_;
  std::function<void(u64)> dialogue_sink_;
  std::function<std::vector<ActorState>()> actor_source_;
  std::function<void(u32)> client_ready_sink_;
  std::function<void(u32)> client_joined_sink_;
  std::function<void(u32)> client_left_sink_;
  struct ActivationWindow {
    u64 start_tick = 0;
    u32 requests = 0;
    bool initialized = false;
    bool warned = false;
  };
  std::unordered_map<u32, ActivationWindow> activation_windows_;
  QuestReplicator quest_replicator_;
  ActorReplicator actor_replicator_;
  std::unique_ptr<AssetStreamServer> asset_stream_;
  u64 tick_ = 0;
};

class GameClientSession final : public Session {
 public:
  explicit GameClientSession(GameSessionConfig config);
  ~GameClientSession() override;

  bool Start();
  void Tick(ecs::World& world, f32 dt) override;

  // Local input forwarded to the server every tick once joined.
  void SetInput(const PlayerInput& input) { inner_.SetInput(input); }

  // Sends an activation request for `handle` to the server (reliable). The
  // server is authoritative for the response (dialogue/quests).
  void SendActivate(u64 handle);
  // Sends the chosen dialogue INFO handle to the server.
  void SendDialogueSelect(u64 info);
  // Asks the server to apply a quest-debugger change.
  void SendStageRequest(const StageRequest& req);

  // Sink invoked once per quest in every kQuestUpdate received.
  void SetQuestSink(std::function<void(u8 domain, const quest::QuestStatus&)> sink) {
    quest_sink_ = std::move(sink);
  }

  // Sink invoked with the command list from every kWorldCommands received.
  void SetWorldCommandSink(std::function<void(const std::vector<world::WorldCommand>&)> sink) {
    world_command_sink_ = std::move(sink);
  }

  // Sink invoked with the NPC transforms in each kActorSync received.
  void SetActorSink(std::function<void(const std::vector<ActorState>&)> sink) {
    actor_sink_ = std::move(sink);
  }

  // Sink invoked once per kObjectiveMarker received.
  void SetObjectiveMarkerSink(std::function<void(const ObjectiveMarkerState&)> sink) {
    objective_marker_sink_ = std::move(sink);
  }

  // Sink invoked once per kWarMap received.
  void SetWarMapSink(std::function<void(const WarMapState&)> sink) {
    war_map_sink_ = std::move(sink);
  }

  // The client's scripting RPC channel. Always present once Start succeeds.
  RpcClientChannel* rpc() { return inner_.rpc(); }

  // The asset-stream downloader, or null when streaming is off.
  AssetStreamClient* asset_stream() { return asset_stream_.get(); }

  // The engine session underneath (replicated bubbles for the visualizer,
  // raw sends).
  ClientSession& engine() { return inner_; }
  const ClientSession& engine() const { return inner_; }

  bool joined() const { return inner_.joined(); }
  u64 player_net_id() const { return inner_.player_net_id(); }
  ecs::Entity player_entity() const { return inner_.player_entity(); }
  u32 replicated_entity_count() const { return inner_.replicated_entity_count(); }

 private:
  void OnGameMessage(u16 type, const u8* data, size_t size);

  GameSessionConfig config_;
  ClientSession inner_;
  std::unique_ptr<AssetStreamClient> asset_stream_;
  std::function<void(u8 domain, const quest::QuestStatus&)> quest_sink_;
  std::function<void(const ObjectiveMarkerState&)> objective_marker_sink_;
  std::function<void(const WarMapState&)> war_map_sink_;
  std::function<void(const std::vector<world::WorldCommand>&)> world_command_sink_;
  std::function<void(const std::vector<ActorState>&)> actor_sink_;
};

}  // namespace rx::net

#endif  // RECREATION_GAMENET_SESSION_H_
