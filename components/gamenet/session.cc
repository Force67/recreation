#include "components/gamenet/session.h"

#include <base/algorithm.h>
#include <nanobuf.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "components/bethesda/form_id.h"
#include "components/gamenet/asset_stream.h"
#include "components/gamenet/player_sync.h"
#include "components/gamenet/world_replication.h"
#include "components/modstream/content_store.h"
#include "components/modstream/mod_catalog.h"
#include "components/world/components.h"
#include "core/log.h"

namespace rx::net {
namespace {

SessionConfig EngineConfig(const GameSessionConfig& game) {
  SessionConfig config;
  config.port = game.port;
  config.address = game.address;
  config.player_name = game.player_name;
  config.protocol = kGameProtocolVersion;
  config.max_clients = game.max_clients;
  config.tick_rate = game.tick_rate;
  config.snapshot_interval_ticks = game.snapshot_interval_ticks;
  config.keyframe_interval_ticks = game.keyframe_interval_ticks;
  config.client_timeout_seconds = game.client_timeout_seconds;
  config.player_mesh = game.player_mesh;
  config.bubble_radius = game.bubble_radius;
  return config;
}

// Recreation's per-entity replication payload is the Bethesda form id, packed
// into the engine's opaque user tag (0 = no form).
ReplicationHooks GameHooks() {
  ReplicationHooks hooks;
  hooks.capture_user_tag = [](ecs::World& world, ecs::Entity entity) -> u64 {
    if (const auto* link = world.Get<world::FormLink>(entity)) {
      return link->form.packed();
    }
    return 0;
  };
  hooks.on_replica_spawned = [](ecs::World& world, ecs::Entity entity, u64 tag) {
    world.Add(entity, world::FormLink{bethesda::GlobalFormId{static_cast<u16>(tag >> 32),
                                                             static_cast<u32>(tag)}});
  };
  return hooks;
}

template <typename Send>
void SendWorldCommandChunks(const base::Vector<world::WorldCommand>& commands, Send&& send) {
  for (mem_size begin = 0; begin < commands.size(); begin += kMaxWorldCommandsPerMessage) {
    const mem_size end = base::Min<mem_size>(commands.size(), begin + kMaxWorldCommandsPerMessage);
    std::vector<world::WorldCommand> chunk(commands.begin() + begin, commands.begin() + end);
    std::vector<u8> payload = EncodeWorldCommands(chunk);
    if (payload.size() <= kMaxWorldCommandPayload)
      send(std::move(payload));
  }
}

}  // namespace

// --- server ---

GameServerSession::GameServerSession(GameSessionConfig config)
    : config_(std::move(config)), inner_(EngineConfig(config_)) {
  inner_.SetReplicationHooks(GameHooks());
  inner_.SetGameMessageSink([this](u32 peer, u16 type, const u8* data, size_t size) {
    OnGameMessage(peer, type, data, size);
  });
  inner_.SetClientJoinedSink([this](u32 peer) {
    // Make sure the newcomer gets the whole journal: quest deltas reach every
    // peer, but resending all of them is cheap and the only way a fresh
    // client gets quests it already missed.
    quest_replicator_.ForceFull();
    if (world_command_source_) {
      SendWorldCommandChunks(world_command_source_(), [this, peer](std::vector<u8> payload) {
        inner_.SendTo(peer, static_cast<u16>(GameMessage::kWorldCommands), payload,
                      /*reliable=*/true, tx::network::PacketPriority::Medium);
      });
    }
    // Offer the mod manifest right after admitting the peer, so it can start
    // streaming whatever content it is missing, along with which of those
    // files are client assemblies the server would like it to run.
    if (asset_stream_) {
      asset_stream_->SendManifest(peer);
      asset_stream_->SendClientScripts(peer);
    }
    // Catch the newcomer up on everyone's replicated vitals.
    for (const auto& [joined_peer, vitals] : player_vitals_) {
      if (!vitals.sent)
        continue;
      inner_.SendTo(peer, static_cast<u16>(GameMessage::kPlayerState),
                    EncodePlayerVitals({engine().PlayerNetId(joined_peer), vitals.health,
                                        vitals.max_health, vitals.dead}),
                    /*reliable=*/true, tx::network::PacketPriority::Medium);
    }
    // And on what time it is, so it loads into the host's hour rather than its
    // own and then jumps on the first beat.
    if (world_state_valid_) {
      inner_.SendTo(peer, static_cast<u16>(GameMessage::kWorldState),
                    EncodeWorldState(world_state_),
                    /*reliable=*/true, tx::network::PacketPriority::Medium);
    }
    if (client_joined_sink_)
      client_joined_sink_(peer);
  });
  inner_.SetClientLeftSink([this](u32 peer) {
    activation_windows_.erase(peer);
    player_vitals_.erase(peer);
    last_swing_seconds_.erase(peer);
    if (client_left_sink_)
      client_left_sink_(peer);
  });
}

GameServerSession::~GameServerSession() = default;

void GameServerSession::SetClientJoinedSink(std::function<void(u32)> sink) {
  client_joined_sink_ = std::move(sink);
}

bool GameServerSession::Start() {
  if (!inner_.Start())
    return false;
  if (config_.mod_catalog) {
    asset_stream_ = std::make_unique<AssetStreamServer>(inner_.raw(), *config_.mod_catalog);
  }
  return true;
}

void GameServerSession::Tick(ecs::World& world, f32 dt) {
  inner_.Tick(world, dt);
  ++tick_;
  clock_seconds_ += static_cast<f64>(dt);
  // Every tick, not on the snapshot cadence: it self-throttles, and a clock the
  // host just moved should reach clients now rather than up to a snapshot later.
  BroadcastWorldState(dt);
  if (tick_ % config_.snapshot_interval_ticks == 0) {
    BroadcastQuests();
    BroadcastActors();
    BroadcastWarMap();
  }
}

void GameServerSession::OnGameMessage(u32 peer, u16 type, const u8* data, size_t size) {
  switch (static_cast<GameMessage>(type)) {
    case GameMessage::kActivateRef: {
      // The payload is a single little-endian u64 form handle.
      if (!activate_sink_ || size != sizeof(u64) || inner_.PlayerOf(peer) == ecs::kInvalidEntity)
        break;
      const u64 handle = nanobuf::LoadLe<u64>(data);
      if (handle == 0)
        break;

      ActivationWindow& window = activation_windows_[peer];
      const u64 window_ticks = std::max<u64>(1, config_.tick_rate);
      if (!window.initialized || tick_ - window.start_tick >= window_ticks) {
        window = {.start_tick = tick_, .requests = 0, .initialized = true, .warned = false};
      }
      if (window.requests >= kMaxActivationRequestsPerSecond) {
        if (!window.warned) {
          RX_WARN("net: activation rate limit reached for peer {}", peer);
          window.warned = true;
        }
        break;
      }
      ++window.requests;
      activate_sink_(peer, handle);
      break;
    }
    case GameMessage::kPlayerAttack: {
      if (!player_attack_sink_ || inner_.PlayerOf(peer) == ecs::kInvalidEntity)
        break;
      const auto yaw = DecodePlayerAttack(data, size);
      if (!yaw)
        break;
      // One cadence between accepted swings. This is the rate limit and the
      // game rule at once: nobody swings faster than the animation, so a client
      // spamming the message gains nothing by it.
      f64& last = last_swing_seconds_[peer];
      if (last != 0.0 && clock_seconds_ - last < static_cast<f64>(swing_cadence_seconds_))
        break;
      last = clock_seconds_;
      player_attack_sink_(peer, *yaw);
      break;
    }
    case GameMessage::kDialogueSelect: {
      if (!dialogue_sink_ || size < 8)
        break;
      dialogue_sink_(nanobuf::LoadLe<u64>(data));
      break;
    }
    case GameMessage::kStageRequest: {
      if (!stage_request_sink_)
        break;
      if (auto req = DecodeStageRequest(ByteSpan(data, size)))
        stage_request_sink_(*req);
      break;
    }
    case GameMessage::kAssetRequest: {
      if (asset_stream_)
        asset_stream_->HandleRequest(peer, data, size);
      break;
    }
    case GameMessage::kAssetReady: {
      if (client_ready_sink_)
        client_ready_sink_(peer);
      break;
    }
    default:
      RX_WARN("net: unhandled game message type {} from peer {}", type, peer);
      break;
  }
}

void GameServerSession::BroadcastQuests() {
  if (!quest_source_ || inner_.client_count() == 0)
    return;
  // The wire codec is std-typed; the source is recreation-side and base-typed.
  const base::Vector<DomainQuestStatus> snapshot = quest_source_();
  std::vector<u8> blob =
      quest_replicator_.Build(std::vector<DomainQuestStatus>(snapshot.begin(), snapshot.end()));
  if (blob.empty())
    return;  // nothing changed this tick

  // Unlike snapshots, quest progress must not be lost, so it rides the
  // reliable channel: a dropped delta would leave a client's journal stale
  // until the next change happens to touch the same quest.
  inner_.Broadcast(static_cast<u16>(GameMessage::kQuestUpdate), blob,
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

void GameServerSession::BroadcastActors() {
  if (!actor_source_ || inner_.client_count() == 0)
    return;
  std::vector<ActorState> changed = actor_replicator_.Build(actor_source_());
  if (changed.empty())
    return;  // no NPC moved this tick
  // Unreliable like snapshots: the next update supersedes a lost one, and the
  // client interpolates between them.
  inner_.Broadcast(static_cast<u16>(GameMessage::kActorSync), EncodeActorStates(changed),
                   /*reliable=*/false, tx::network::PacketPriority::Medium);
}

void GameServerSession::BroadcastWarMap() {
  if (!war_map_source_ || inner_.client_count() == 0)
    return;
  std::vector<u8> blob = EncodeWarMap(war_map_source_());
  // Skip unchanged ticks, but always re-send when a new client joins so a late
  // joiner gets the current front rather than waiting for the next capture.
  if (blob == last_war_map_blob_ && inner_.client_count() == last_war_map_clients_)
    return;
  last_war_map_blob_ = blob;
  last_war_map_clients_ = inner_.client_count();
  inner_.Broadcast(static_cast<u16>(GameMessage::kWarMap), std::move(blob),
                   /*reliable=*/true, tx::network::PacketPriority::Low);
}

void GameServerSession::SendWorldCommands(const base::Vector<world::WorldCommand>& commands) {
  if (inner_.client_count() == 0 || commands.empty())
    return;
  // Reliable, like quests: a dropped spawn or cleanup would desync a client's
  // world from the host's permanently.
  SendWorldCommandChunks(commands, [this](std::vector<u8> payload) {
    inner_.Broadcast(static_cast<u16>(GameMessage::kWorldCommands), payload,
                     /*reliable=*/true, tx::network::PacketPriority::Medium);
  });
}

void GameServerSession::SendObjectiveMarker(const ObjectiveMarkerState& m) {
  if (inner_.client_count() == 0)
    return;
  // Reliable: a dropped marker would leave the clients' compass pip stale until
  // the next change.
  inner_.Broadcast(static_cast<u16>(GameMessage::kObjectiveMarker), EncodeObjectiveMarker(m),
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

void GameServerSession::BroadcastWorldState(f32 dt) {
  if (!world_state_source_)
    return;
  const WorldState state = world_state_source_();
  world_state_age_ += dt;

  // With nobody connected there is nothing to tell, but the sample is kept
  // current so the next joiner is admitted into this hour rather than into
  // whatever hour the last client was told about.
  if (inner_.client_count() == 0) {
    world_state_ = state;
    world_state_valid_ = true;
    world_state_age_ = 0.0f;
    return;
  }

  // A client runs its own clock forward from the last message at the timescale
  // that message carried, so ordinary passing time is not news. What is: a
  // different sky, a different rate, a clock the host moved out from under that
  // extrapolation (a script set the hour), and a slow heartbeat to mop up drift
  // and cover a client that somehow missed a change.
  constexpr f32 kHeartbeatSeconds = 5.0f;
  constexpr f64 kClockToleranceDays = 10.0 / 86400.0;  // ten game seconds
  const f64 expected =
      world_state_.game_days +
      static_cast<f64>(world_state_age_) * static_cast<f64>(state.timescale) / 86400.0;
  const bool news = !world_state_valid_ || state.weather_seed != world_state_.weather_seed ||
                    state.weather != world_state_.weather ||
                    state.timescale != world_state_.timescale ||
                    std::abs(state.game_days - expected) > kClockToleranceDays ||
                    world_state_age_ >= kHeartbeatSeconds;
  if (!news)
    return;

  world_state_ = state;
  world_state_valid_ = true;
  world_state_age_ = 0.0f;
  inner_.Broadcast(static_cast<u16>(GameMessage::kWorldState), EncodeWorldState(state),
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

void GameServerSession::SetPlayerHealth(u32 peer, u16 health, u16 max_health, bool dead) {
  if (inner_.PlayerNetId(peer) == 0)
    return;  // an unknown or departed peer has no body to announce vitals for
  PlayerVitalsEntry& entry = player_vitals_[peer];
  if (entry.sent && entry.health == health && entry.max_health == max_health &&
      entry.dead == dead)
    return;  // unchanged: keep quiet (the wire is not a per-frame cost)
  entry.health = health;
  entry.max_health = max_health;
  entry.dead = dead;
  entry.sent = true;
  inner_.Broadcast(static_cast<u16>(GameMessage::kPlayerState),
                   EncodePlayerVitals({engine().PlayerNetId(peer), health, max_health, dead}),
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

bool GameServerSession::PlayerVitalsOf(u32 peer,
                                       u16* health,
                                       u16* max_health,
                                       bool* dead) const {
  const auto it = player_vitals_.find(peer);
  if (it == player_vitals_.end() || !it->second.sent)
    return false;
  if (health)
    *health = it->second.health;
  if (max_health)
    *max_health = it->second.max_health;
  if (dead)
    *dead = it->second.dead;
  return true;
}

u64 GameServerSession::PlayerNetId(u32 peer) const {
  return inner_.PlayerNetId(peer);
}

bool GameServerSession::Kick(u32 peer) {
  bool known = false;
  inner_.ForEachPeer([&](u32 joined) { known = known || joined == peer; });
  if (!known)
    return false;
  // The transport's goodbye is what a client acts on: it disconnects as soon as
  // it arrives, connected or not. There is no "kicked" reason in the protocol's
  // list, and None is the one a client reports as "the server rejected you",
  // which is what a kick is.
  inner_.raw().SendServerGoodbye(static_cast<tx::network::ZPeerId>(peer),
                                 tx::network::system_commands::HandshakeRejectReason::None);
  player_vitals_.erase(peer);
  RX_INFO("net: kicked peer {}", peer);
  return true;
}

void GameServerSession::ReloadCatalog(const modstream::ModCatalog& catalog) {
  if (!asset_stream_)
    return;
  asset_stream_->SetCatalog(catalog);
  // Push the new manifest to everyone already connected; each re-diffs against
  // its cache and streams only what changed, then re-mounts. The script offer
  // rides along so a changed assembly list is known, though newly loaded code
  // applies on the next join.
  inner_.ForEachPeer([this](u32 peer) {
    asset_stream_->SendManifest(peer);
    asset_stream_->SendClientScripts(peer);
  });
}

// --- client ---

GameClientSession::GameClientSession(GameSessionConfig config)
    : config_(std::move(config)), inner_(EngineConfig(config_)) {
  inner_.SetReplicationHooks(GameHooks());
  inner_.SetGameMessageSink(
      [this](u16 type, const u8* data, size_t size) { OnGameMessage(type, data, size); });
}

GameClientSession::~GameClientSession() = default;

bool GameClientSession::Start() {
  if (!inner_.Start())
    return false;
  if (config_.content_store) {
    asset_stream_ = std::make_unique<AssetStreamClient>(
        inner_.raw(), *config_.content_store, config_.content_store->root() / ".incoming");
    inner_.SetFilePacketSink(
        [this](const tx::network::IncomingPacket& packet) { asset_stream_->OnFilePacket(packet); });
  }
  return true;
}

void GameClientSession::Tick(ecs::World& world, f32 dt) {
  inner_.Tick(world, dt);
}

void GameClientSession::SendActivate(u64 handle) {
  if (!joined())
    return;
  std::vector<u8> payload(8);
  nanobuf::StoreLe<u64>(payload.data(), handle);
  inner_.SendToServer(static_cast<u16>(GameMessage::kActivateRef), payload,
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::SendAttack(f32 yaw) {
  if (!joined())
    return;
  inner_.SendToServer(static_cast<u16>(GameMessage::kPlayerAttack), EncodePlayerAttack(yaw),
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::SendDialogueSelect(u64 info) {
  if (!joined())
    return;
  std::vector<u8> payload(8);
  nanobuf::StoreLe<u64>(payload.data(), info);
  inner_.SendToServer(static_cast<u16>(GameMessage::kDialogueSelect), payload,
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::SendStageRequest(const StageRequest& req) {
  if (!joined())
    return;
  inner_.SendToServer(static_cast<u16>(GameMessage::kStageRequest), EncodeStageRequest(req),
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::OnGameMessage(u16 type, const u8* data, size_t size) {
  const ByteSpan blob(data, size);
  switch (static_cast<GameMessage>(type)) {
    case GameMessage::kQuestUpdate: {
      if (!quest_sink_)
        break;
      if (!ApplyQuestUpdate(blob, quest_sink_)) {
        RX_WARN("net: dropped corrupt quest update");
      }
      break;
    }
    case GameMessage::kWorldCommands: {
      if (!world_command_sink_)
        break;
      if (auto cmds = DecodeWorldCommands(blob)) {
        world_command_sink_(*cmds);
      } else {
        RX_WARN("net: dropped corrupt world-command update");
      }
      break;
    }
    case GameMessage::kActorSync: {
      if (!actor_sink_)
        break;
      if (auto actors = DecodeActorStates(blob)) {
        actor_sink_(*actors);
      } else {
        RX_WARN("net: dropped corrupt actor sync");
      }
      break;
    }
    case GameMessage::kObjectiveMarker: {
      if (!objective_marker_sink_)
        break;
      if (auto marker = DecodeObjectiveMarker(blob)) {
        objective_marker_sink_(*marker);
      } else {
        RX_WARN("net: dropped corrupt objective marker");
      }
      break;
    }
    case GameMessage::kWarMap: {
      if (!war_map_sink_)
        break;
      if (auto board = DecodeWarMap(blob)) {
        war_map_sink_(*board);
      } else {
        RX_WARN("net: dropped corrupt war map");
      }
      break;
    }
    case GameMessage::kAssetManifest: {
      if (asset_stream_)
        asset_stream_->OnManifestChunk(data, size);
      break;
    }
    case GameMessage::kClientScripts: {
      if (asset_stream_)
        asset_stream_->OnClientScripts(data, size);
      break;
    }
    case GameMessage::kPlayerAvatar: {
      if (player_avatar_sink_)
        if (auto avatar = DecodePlayerAvatar(data, size))
          player_avatar_sink_(avatar->net_id, avatar->form);
      break;
    }
    case GameMessage::kPlayerState: {
      if (player_vitals_sink_)
        if (auto vitals = DecodePlayerVitals(data, size))
          player_vitals_sink_(vitals->net_id, vitals->health, vitals->max_health, vitals->dead);
      break;
    }
    case GameMessage::kWorldState: {
      if (world_state_sink_)
        if (auto state = DecodeWorldState(data, size))
          world_state_sink_(*state);
      break;
    }
    default:
      RX_WARN("net: unhandled game message type {}", type);
      break;
  }
}

}  // namespace rx::net
