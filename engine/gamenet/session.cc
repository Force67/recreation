#include "gamenet/session.h"

#include <nanobuf.h>

#include <algorithm>

#include "bethesda/form_id.h"
#include "core/log.h"
#include "gamenet/asset_stream.h"
#include "gamenet/world_replication.h"
#include "modstream/content_store.h"
#include "modstream/mod_catalog.h"
#include "world/components.h"

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
void SendWorldCommandChunks(const std::vector<world::WorldCommand>& commands, Send&& send) {
  for (size_t begin = 0; begin < commands.size(); begin += kMaxWorldCommandsPerMessage) {
    const size_t end = std::min(commands.size(), begin + kMaxWorldCommandsPerMessage);
    std::vector<world::WorldCommand> chunk(commands.begin() + begin, commands.begin() + end);
    std::vector<u8> payload = EncodeWorldCommands(chunk);
    if (payload.size() <= kMaxWorldCommandPayload) send(std::move(payload));
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
    // streaming whatever content it is missing.
    if (asset_stream_) asset_stream_->SendManifest(peer);
    if (client_joined_sink_) client_joined_sink_(peer);
  });
  inner_.SetClientLeftSink([this](u32 peer) {
    activation_windows_.erase(peer);
    if (client_left_sink_) client_left_sink_(peer);
  });
}

GameServerSession::~GameServerSession() = default;

void GameServerSession::SetClientJoinedSink(std::function<void(u32)> sink) {
  client_joined_sink_ = std::move(sink);
}

bool GameServerSession::Start() {
  if (!inner_.Start()) return false;
  if (config_.mod_catalog) {
    asset_stream_ = std::make_unique<AssetStreamServer>(inner_.raw(), *config_.mod_catalog);
  }
  return true;
}

void GameServerSession::Tick(ecs::World& world, f32 dt) {
  inner_.Tick(world, dt);
  ++tick_;
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
      if (handle == 0) break;

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
    case GameMessage::kDialogueSelect: {
      if (!dialogue_sink_ || size < 8) break;
      dialogue_sink_(nanobuf::LoadLe<u64>(data));
      break;
    }
    case GameMessage::kStageRequest: {
      if (!stage_request_sink_) break;
      if (auto req = DecodeStageRequest(ByteSpan(data, size))) stage_request_sink_(*req);
      break;
    }
    case GameMessage::kAssetRequest: {
      if (asset_stream_) asset_stream_->HandleRequest(peer, data, size);
      break;
    }
    case GameMessage::kAssetReady: {
      if (client_ready_sink_) client_ready_sink_(peer);
      break;
    }
    default:
      RX_WARN("net: unhandled game message type {} from peer {}", type, peer);
      break;
  }
}

void GameServerSession::BroadcastQuests() {
  if (!quest_source_ || inner_.client_count() == 0) return;
  std::vector<u8> blob = quest_replicator_.Build(quest_source_());
  if (blob.empty()) return;  // nothing changed this tick

  // Unlike snapshots, quest progress must not be lost, so it rides the
  // reliable channel: a dropped delta would leave a client's journal stale
  // until the next change happens to touch the same quest.
  inner_.Broadcast(static_cast<u16>(GameMessage::kQuestUpdate), blob,
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

void GameServerSession::BroadcastActors() {
  if (!actor_source_ || inner_.client_count() == 0) return;
  std::vector<ActorState> changed = actor_replicator_.Build(actor_source_());
  if (changed.empty()) return;  // no NPC moved this tick
  // Unreliable like snapshots: the next update supersedes a lost one, and the
  // client interpolates between them.
  inner_.Broadcast(static_cast<u16>(GameMessage::kActorSync), EncodeActorStates(changed),
                   /*reliable=*/false, tx::network::PacketPriority::Medium);
}

void GameServerSession::BroadcastWarMap() {
  if (!war_map_source_ || inner_.client_count() == 0) return;
  std::vector<u8> blob = EncodeWarMap(war_map_source_());
  // Skip unchanged ticks, but always re-send when a new client joins so a late
  // joiner gets the current front rather than waiting for the next capture.
  if (blob == last_war_map_blob_ && inner_.client_count() == last_war_map_clients_) return;
  last_war_map_blob_ = blob;
  last_war_map_clients_ = inner_.client_count();
  inner_.Broadcast(static_cast<u16>(GameMessage::kWarMap), std::move(blob),
                   /*reliable=*/true, tx::network::PacketPriority::Low);
}

void GameServerSession::SendWorldCommands(const std::vector<world::WorldCommand>& commands) {
  if (inner_.client_count() == 0 || commands.empty()) return;
  // Reliable, like quests: a dropped spawn or cleanup would desync a client's
  // world from the host's permanently.
  SendWorldCommandChunks(commands, [this](std::vector<u8> payload) {
    inner_.Broadcast(static_cast<u16>(GameMessage::kWorldCommands), payload,
                     /*reliable=*/true, tx::network::PacketPriority::Medium);
  });
}

void GameServerSession::SendObjectiveMarker(const ObjectiveMarkerState& m) {
  if (inner_.client_count() == 0) return;
  // Reliable: a dropped marker would leave the clients' compass pip stale until
  // the next change.
  inner_.Broadcast(static_cast<u16>(GameMessage::kObjectiveMarker), EncodeObjectiveMarker(m),
                   /*reliable=*/true, tx::network::PacketPriority::Medium);
}

void GameServerSession::ReloadCatalog(const modstream::ModCatalog& catalog) {
  if (!asset_stream_) return;
  asset_stream_->SetCatalog(catalog);
  // Push the new manifest to everyone already connected; each re-diffs against
  // its cache and streams only what changed, then re-mounts.
  inner_.ForEachPeer([this](u32 peer) { asset_stream_->SendManifest(peer); });
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
  if (!inner_.Start()) return false;
  if (config_.content_store) {
    asset_stream_ = std::make_unique<AssetStreamClient>(
        inner_.raw(), *config_.content_store, config_.content_store->root() / ".incoming");
    inner_.SetFilePacketSink(
        [this](const tx::network::IncomingPacket& packet) { asset_stream_->OnFilePacket(packet); });
  }
  return true;
}

void GameClientSession::Tick(ecs::World& world, f32 dt) { inner_.Tick(world, dt); }

void GameClientSession::SendActivate(u64 handle) {
  if (!joined()) return;
  std::vector<u8> payload(8);
  nanobuf::StoreLe<u64>(payload.data(), handle);
  inner_.SendToServer(static_cast<u16>(GameMessage::kActivateRef), payload,
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::SendDialogueSelect(u64 info) {
  if (!joined()) return;
  std::vector<u8> payload(8);
  nanobuf::StoreLe<u64>(payload.data(), info);
  inner_.SendToServer(static_cast<u16>(GameMessage::kDialogueSelect), payload,
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::SendStageRequest(const StageRequest& req) {
  if (!joined()) return;
  inner_.SendToServer(static_cast<u16>(GameMessage::kStageRequest), EncodeStageRequest(req),
                      /*reliable=*/true, tx::network::PacketPriority::High);
}

void GameClientSession::OnGameMessage(u16 type, const u8* data, size_t size) {
  const ByteSpan blob(data, size);
  switch (static_cast<GameMessage>(type)) {
    case GameMessage::kQuestUpdate: {
      if (!quest_sink_) break;
      if (!ApplyQuestUpdate(blob, quest_sink_)) {
        RX_WARN("net: dropped corrupt quest update");
      }
      break;
    }
    case GameMessage::kWorldCommands: {
      if (!world_command_sink_) break;
      if (auto cmds = DecodeWorldCommands(blob)) {
        world_command_sink_(*cmds);
      } else {
        RX_WARN("net: dropped corrupt world-command update");
      }
      break;
    }
    case GameMessage::kActorSync: {
      if (!actor_sink_) break;
      if (auto actors = DecodeActorStates(blob)) {
        actor_sink_(*actors);
      } else {
        RX_WARN("net: dropped corrupt actor sync");
      }
      break;
    }
    case GameMessage::kObjectiveMarker: {
      if (!objective_marker_sink_) break;
      if (auto marker = DecodeObjectiveMarker(blob)) {
        objective_marker_sink_(*marker);
      } else {
        RX_WARN("net: dropped corrupt objective marker");
      }
      break;
    }
    case GameMessage::kWarMap: {
      if (!war_map_sink_) break;
      if (auto board = DecodeWarMap(blob)) {
        war_map_sink_(*board);
      } else {
        RX_WARN("net: dropped corrupt war map");
      }
      break;
    }
    case GameMessage::kAssetManifest: {
      if (asset_stream_) asset_stream_->OnManifestChunk(data, size);
      break;
    }
    default:
      RX_WARN("net: unhandled game message type {}", type);
      break;
  }
}

}  // namespace rx::net
