#ifndef RECREATION_GAMENET_SESSION_H_
#define RECREATION_GAMENET_SESSION_H_

// Recreation's thin session layer over rx::net. The engine module owns the
// transport, the join handshake, per-peer entity streams and the streaming
// bubbles (interest + ownership); these wrappers add only what is game:
// quest/war-map/actor replication, dialogue/stage/activate routing, and the
// mod asset streaming. Everything crosses the wire as GameMessage payloads
// through the sessions' game-message seams.

#include <base/containers/vector.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "components/gamenet/actor_sync.h"
#include "components/gamenet/item_sync.h"
#include "components/gamenet/objective_marker_net.h"
#include "components/gamenet/protocol.h"
#include "components/gamenet/quest_replication.h"
#include "components/gamenet/stage_request.h"
#include "components/gamenet/war_map_net.h"
#include "components/gamenet/world_state.h"
#include "components/quest/quest_system.h"
#include "components/world/quest_world.h"
#include "ecs/world.h"
#include "net/session.h"

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
  void SetQuestSource(std::function<base::Vector<DomainQuestStatus>()> source) {
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
  void SendWorldCommands(const base::Vector<world::WorldCommand>& commands);
  void SetWorldCommandSource(std::function<base::Vector<world::WorldCommand>()> source) {
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

  // Sink invoked with the peer and the aim of every swing a client throws that
  // the host accepts. Swings arriving faster than one melee cadence are dropped
  // before the sink sees them: the cadence is both the rate limit and the game
  // rule, since nobody can swing faster than the animation.
  void SetPlayerAttackSink(std::function<void(u32 peer, f32 yaw)> sink) {
    player_attack_sink_ = std::move(sink);
  }
  // Seconds a peer must wait between accepted swings.
  void set_swing_cadence_seconds(f32 seconds) { swing_cadence_seconds_ = seconds; }

  // Sink invoked with the peer each time a client asks to drop its most recent
  // stack. What it is carrying is the host's own record, so the request carries
  // nothing but the asking.
  void SetItemDropSink(std::function<void(u32 peer)> sink) { item_drop_sink_ = std::move(sink); }

  // Announces a piece of loot: which replicated entity it is and the base record
  // it came out of. Remembered so a joining client is told about the loot already
  // on the floor, and forgotten when the host says the item is gone.
  void SendWorldItem(const WorldItemState& item);
  void ForgetWorldItem(u64 net_id);

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

  // The authoritative vitals for a player: the host's combat resolution writes
  // them, and so can host-side mod code through the Player.SetHealth SDK API.
  // Broadcasts a kPlayerState to every client on change and remembers the value
  // so a later joiner's table is complete.
  void SetPlayerHealth(u32 peer, u16 health, u16 max_health, bool dead);
  // The vitals last announced for a peer, for a caller that has to change them
  // relative to what they are (combat subtracting from a health pool). False
  // when this peer has never had any, which is also how "not a player here"
  // reads.
  bool PlayerVitalsOf(u32 peer, u16* health, u16* max_health, bool* dead) const;
  // The player entity's network id, once the peer has joined; 0 before.
  u64 PlayerNetId(u32 peer) const;

  // Drops a peer: the transport says goodbye and the client disconnects on the
  // spot. The server's own roster clears when the peer times out a few seconds
  // later (nothing acknowledges a goodbye, so there is nothing sooner to go on),
  // which is also what makes a kick survive a client that ignores it. False when
  // the peer is not one of ours.
  bool Kick(u32 peer);

  // The shared clock and sky (see world_state.h), sampled every tick. Only what
  // a client cannot derive for itself goes out: a changed seed, weather or
  // timescale, a clock the host moved out from under the client's own
  // extrapolation, and a slow heartbeat that mops up drift. The last state sent
  // is remembered so a joining client is told the time as it is admitted rather
  // than on the next beat.
  void SetWorldStateSource(std::function<WorldState()> source) {
    world_state_source_ = std::move(source);
  }

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
  void BroadcastWorldState(f32 dt);

  GameSessionConfig config_;
  ServerSession inner_;
  std::function<base::Vector<DomainQuestStatus>()> quest_source_;
  std::function<base::Vector<world::WorldCommand>()> world_command_source_;
  std::function<WarMapState()> war_map_source_;
  std::vector<u8> last_war_map_blob_;  // last board sent, to skip unchanged ticks
  size_t last_war_map_clients_ = 0;    // re-send the board when a new client joins
  std::function<void(const StageRequest&)> stage_request_sink_;
  std::function<void(u32, u64)> activate_sink_;
  std::function<void(u64)> dialogue_sink_;
  std::function<void(u32 peer, f32 yaw)> player_attack_sink_;
  std::function<void(u32 peer)> item_drop_sink_;
  // Loot the host has told clients about, so a joiner can be caught up.
  std::unordered_map<u64, u64> world_items_;  // net id -> base form
  // Server clock (seconds since start) of each peer's last accepted swing, and
  // the cadence they are held to.
  std::unordered_map<u32, f64> last_swing_seconds_;
  f32 swing_cadence_seconds_ = 1.3f;
  f64 clock_seconds_ = 0.0;
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
  struct PlayerVitalsEntry {
    u16 health = 0;
    u16 max_health = 0;
    bool dead = false;
    bool sent = false;  // a vitals message has gone out for this player
  };
  std::unordered_map<u32, PlayerVitalsEntry> player_vitals_;
  // The last world state broadcast, whether one ever was, and the real seconds
  // since: the host samples its clock every tick and only what a client cannot
  // extrapolate goes out.
  std::function<WorldState()> world_state_source_;
  WorldState world_state_;
  bool world_state_valid_ = false;
  f32 world_state_age_ = 0.0f;

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
  // Asks the server to resolve a swing aimed along `yaw`. The client never
  // decides what it hit; the host answers with whatever vitals changed.
  void SendAttack(f32 yaw);
  // Asks the server to drop this player's most recent stack. The host owns the
  // pack and the world item that comes out of it.
  void SendItemDrop();
  // Sends the chosen dialogue INFO handle to the server.
  void SendDialogueSelect(u64 info);
  // Asks the server to apply a quest-debugger change.
  void SendStageRequest(const StageRequest& req);

  // Sink invoked once per quest in every kQuestUpdate received.
  void SetQuestSink(std::function<void(u8 domain, const quest::QuestStatus&)> sink) {
    quest_sink_ = std::move(sink);
  }

  // Sink invoked with the command list from every kWorldCommands received.
  void SetWorldCommandSink(std::function<void(const base::Vector<world::WorldCommand>&)> sink) {
    world_command_sink_ = std::move(sink);
  }

  // Sink invoked with the NPC transforms in each kActorSync received.
  void SetActorSink(std::function<void(const base::Vector<ActorState>&)> sink) {
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

  // Sink invoked once per kPlayerAvatar received: which replicated entity is a
  // player's body and the appearance form it carries (0 = the game's default
  // player template). The engine applies it to the replica entity.
  void SetPlayerAvatarSink(std::function<void(u64 net_id, u64 form)> sink) {
    player_avatar_sink_ = std::move(sink);
  }

  // Sink invoked once per kPlayerState received: a player entity's replicated
  // vitals.
  void SetPlayerVitalsSink(std::function<void(u64 net_id, u16 health, u16 max, bool dead)> sink) {
    player_vitals_sink_ = std::move(sink);
  }

  // Sink invoked once per kWorldItem received: which replica is loot, and the
  // base record to render it from.
  void SetWorldItemSink(std::function<void(const WorldItemState&)> sink) {
    world_item_sink_ = std::move(sink);
  }

  // Sink invoked once per kWorldState received: the host's clock and the seed
  // its weather derives from. The engine adopts both.
  void SetWorldStateSink(std::function<void(const WorldState&)> sink) {
    world_state_sink_ = std::move(sink);
  }

  // The replicated entity carrying this network id, or kInvalidEntity when the
  // snapshot has not (yet) spawned it.
  ecs::Entity replicated_entity(u64 net_id) const { return inner_.replicated_entity(net_id); }

  // The client's scripting RPC channel. Always present once Start succeeds.
  RpcClientChannel* rpc() { return inner_.rpc(); }

  // The asset-stream downloader, or null when streaming is off.
  AssetStreamClient* asset_stream() { return (asset_stream_ ? &*asset_stream_ : nullptr); }

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
  std::function<void(u64 net_id, u64 form)> player_avatar_sink_;
  std::function<void(u64 net_id, u16 health, u16 max, bool dead)> player_vitals_sink_;
  std::function<void(const ObjectiveMarkerState&)> objective_marker_sink_;
  std::function<void(const WarMapState&)> war_map_sink_;
  std::function<void(const WorldState&)> world_state_sink_;
  std::function<void(const WorldItemState&)> world_item_sink_;
  std::function<void(const base::Vector<world::WorldCommand>&)> world_command_sink_;
  std::function<void(const base::Vector<ActorState>&)> actor_sink_;
};

}  // namespace rx::net

#endif  // RECREATION_GAMENET_SESSION_H_
