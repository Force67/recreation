#include "net/session.h"

#include <cmath>

#include <nanobuf.h>

#include "core/log.h"
#include "net/world_replication.h"

namespace rec::net {
namespace {

constexpr f32 kPlayerSpeed = 4.0f;  // units per second

f32 ClampAxis(f32 v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

// --- server ---

ServerSession::ServerSession(SessionConfig config) : config_(std::move(config)) {}

ServerSession::~ServerSession() {
  if (started_) server_.Deinit();
}

bool ServerSession::Start() {
  const tx::network::ZServer::StartOptions options{
      .use_encryption = false,
      .pre_shared_key = {},
      .use_compression = false,
      .allow_ipv6 = false,
      // Threaded mode: reliable retransmits and socket receive run in
      // zetanet's workers, the sim thread only touches lock-free queues.
      .start_threads = true,
      .chaos = {}};
  if (!server_.Begin(config_.port, options)) {
    REC_ERROR("net: failed to open server on port {}", config_.port);
    return false;
  }
  started_ = true;
  REC_INFO("net: server listening on {}", config_.port);
  return true;
}

void ServerSession::Tick(ecs::World& world, f32 dt) {
  if (!started_) return;
  server_.Update();
  PollMessages(world);
  SimulatePlayers(world, dt);
  TimeoutClients(world, dt);
  ++tick_;
  if (tick_ % config_.snapshot_interval_ticks == 0) {
    BroadcastSnapshot(world);
    BroadcastQuests();
    BroadcastActors();
  }
  // Heartbeat for dedicated server logs.
  if (tick_ % (static_cast<u64>(config_.tick_rate) * 30) == 0) {
    REC_INFO("net: tick {}, {} clients, {} entities", tick_, clients_.size(),
             world.entity_count());
  }
}

void ServerSession::PollMessages(ecs::World& world) {
  tx::network::IncomingPacket packet;
  while (server_.Poll(tx::network::PacketChannelType::Data, packet)) {
    if (tx::network::IsSystemMessage(packet.type)) continue;
    const u32 peer = packet.source_peer_id;
    if (RemoteClient* client = clients_.find(peer)) {
      client->since_last_packet = 0;
    }
    switch (static_cast<MessageType>(packet.type)) {
      case MessageType::kClientJoin: {
        if (auto join = ParseAs<ClientJoinView>(packet)) {
          HandleJoin(world, peer, *join);
        }
        break;
      }
      case MessageType::kPlayerInput: {
        RemoteClient* client = clients_.find(peer);
        if (!client) break;
        if (auto input = ParseAs<PlayerInputView>(packet)) {
          // Inputs are unreliable and unordered, keep only the newest.
          if (input->client_tick() >= client->input.client_tick) {
            if (auto owned = input->ToOwned()) client->input = *owned;
          }
        }
        break;
      }
      case MessageType::kActivateRef: {
        // The payload is a single little-endian u64 form handle.
        if (!activate_sink_ || packet.data.size() < 8) break;
        const u64 handle =
            nanobuf::LoadLe<u64>(reinterpret_cast<const u8*>(packet.data.data()));
        activate_sink_(handle);
        break;
      }
      case MessageType::kDialogueSelect: {
        if (!dialogue_sink_ || packet.data.size() < 8) break;
        const u64 info = nanobuf::LoadLe<u64>(reinterpret_cast<const u8*>(packet.data.data()));
        dialogue_sink_(info);
        break;
      }
      case MessageType::kStageRequest: {
        if (!stage_request_sink_) break;
        const ByteSpan blob(reinterpret_cast<const u8*>(packet.data.data()), packet.data.size());
        if (auto req = DecodeStageRequest(blob)) stage_request_sink_(*req);
        break;
      }
      default:
        REC_WARN("net: unhandled message type {} from peer {}",
                 static_cast<u16>(packet.type), peer);
        break;
    }
  }
}

void ServerSession::HandleJoin(ecs::World& world, u32 peer,
                               const ClientJoinView& join) {
  if (join.protocol() != kProtocolVersion) {
    JoinRefuse refuse;
    refuse.reason = DisconnectReason::kProtocolMismatch;
    refuse.detail = "protocol version mismatch";
    server_.Push(MakePacket(peer, MessageType::kJoinRefuse, refuse.Encode(),
                            /*reliable=*/true,
                            tx::network::PacketPriority::High));
    return;
  }

  RemoteClient* client = clients_.find(peer);
  if (!client) {
    if (clients_.size() >= config_.max_clients) {
      JoinRefuse refuse;
      refuse.reason = DisconnectReason::kServerFull;
      refuse.detail = "server full";
      server_.Push(MakePacket(peer, MessageType::kJoinRefuse, refuse.Encode(),
                              /*reliable=*/true,
                              tx::network::PacketPriority::High));
      return;
    }

    RemoteClient fresh;
    if (auto name = join.player_name()) {
      fresh.name = base::NameString(name->data(), name->size());
    }
    // The player entity is server-owned and replicates like anything else.
    const NetworkId net_id = AllocateNetworkId();
    world::Transform spawn;
    spawn.position[0] = 1.5f * static_cast<f32>(clients_.size() + 1);
    spawn.position[2] = 1.5f;
    fresh.player = world.Create();
    world.Add(fresh.player, spawn);
    world.Add(fresh.player, net_id);
    if (config_.player_mesh != 0) {
      world.Add(fresh.player, world::Renderable{asset::AssetId{config_.player_mesh}});
    }
    fresh.player_net_id = net_id.value;
    clients_.insert(peer, std::move(fresh));
    client = clients_.find(peer);
    REC_INFO("net: peer {} joined as '{}' ({} clients)", peer,
             client->name.c_str(), clients_.size());
    // Make sure the newcomer gets the whole world (and journal) on the next
    // broadcast. Quest deltas reach every peer, but resending all of them is
    // cheap and the only way a fresh client gets quests it already missed.
    force_keyframe_ = true;
    quest_replicator_.ForceFull();
  }

  // Reply (again, on duplicate joins from retransmits): the accept itself
  // can get lost even on the reliable path if the ack raced a timeout.
  JoinAccept accept;
  accept.player_entity = client->player_net_id;
  accept.server_tick = tick_;
  accept.protocol = kProtocolVersion;
  accept.client_id = peer;
  accept.tick_rate = static_cast<u16>(config_.tick_rate);
  accept.snapshot_rate = static_cast<u16>(
      config_.tick_rate / config_.snapshot_interval_ticks);
  server_.Push(MakePacket(peer, MessageType::kJoinAccept, accept.Encode(),
                          /*reliable=*/true, tx::network::PacketPriority::High));
}

void ServerSession::DropClient(ecs::World& world, u32 peer) {
  RemoteClient* client = clients_.find(peer);
  if (!client) return;
  REC_INFO("net: dropping peer {} ('{}')", peer, client->name.c_str());
  if (world.IsAlive(client->player)) world.Destroy(client->player);
  clients_.erase(peer);
}

void ServerSession::SimulatePlayers(ecs::World& world, f32 dt) {
  for (auto entry : clients_) {
    RemoteClient& client = entry.value;
    world::Transform* t = world.Get<world::Transform>(client.player);
    if (!t) continue;
    t->position[0] += ClampAxis(client.input.move_x) * kPlayerSpeed * dt;
    t->position[1] += ClampAxis(client.input.move_y) * kPlayerSpeed * dt;
    t->position[2] += ClampAxis(client.input.move_z) * kPlayerSpeed * dt;
    const f32 half_yaw = client.input.yaw * 0.5f;
    t->rotation[0] = 0;
    t->rotation[1] = std::sin(half_yaw);
    t->rotation[2] = 0;
    t->rotation[3] = std::cos(half_yaw);
  }
}

void ServerSession::TimeoutClients(ecs::World& world, f32 dt) {
  scratch_dropped_.clear();
  for (auto entry : clients_) {
    entry.value.since_last_packet += dt;
    if (entry.value.since_last_packet > config_.client_timeout_seconds) {
      scratch_dropped_.push_back(entry.key);
    }
  }
  for (u32 peer : scratch_dropped_) DropClient(world, peer);
}

void ServerSession::BroadcastSnapshot(ecs::World& world) {
  if (clients_.size() == 0) {
    // Keep delta state warm so the first client still gets sane despawns.
    builder_.Build(world, tick_, /*full=*/true, &snapshot_);
    return;
  }
  const bool full =
      force_keyframe_ || (tick_ % config_.keyframe_interval_ticks == 0);
  force_keyframe_ = false;
  const u32 written = builder_.Build(world, tick_, full, &snapshot_);
  if (written == 0 && snapshot_.despawned.empty() && !full) return;

  // Snapshots stay unreliable by design: the next one supersedes a lost one
  // and keyframes repair anything structural.
  server_.Push(MakePacket(tx::network::ZPeerId::to_all, MessageType::kSnapshot,
                          snapshot_.Encode(), /*reliable=*/false,
                          full ? tx::network::PacketPriority::High
                               : tx::network::PacketPriority::Medium));
}

void ServerSession::BroadcastQuests() {
  if (!quest_source_ || clients_.size() == 0) return;
  std::vector<u8> blob = quest_replicator_.Build(quest_source_());
  if (blob.empty()) return;  // nothing changed this tick

  // Unlike snapshots, quest progress must not be lost, so it rides the
  // reliable channel: a dropped delta would leave a client's journal stale
  // until the next change happens to touch the same quest.
  server_.Push(MakePacket(tx::network::ZPeerId::to_all,
                          MessageType::kQuestUpdate, blob, /*reliable=*/true,
                          tx::network::PacketPriority::Medium));
}

void ServerSession::BroadcastActors() {
  if (!actor_source_ || clients_.size() == 0) return;
  std::vector<ActorState> changed = actor_replicator_.Build(actor_source_());
  if (changed.empty()) return;  // no NPC moved this tick
  std::vector<u8> blob = EncodeActorStates(changed);
  // Unreliable like snapshots: the next update supersedes a lost one, and the
  // client interpolates between them.
  server_.Push(MakePacket(tx::network::ZPeerId::to_all, MessageType::kActorSync, blob,
                          /*reliable=*/false, tx::network::PacketPriority::Medium));
}

void ServerSession::SendWorldCommands(const std::vector<world::WorldCommand>& commands) {
  if (clients_.size() == 0 || commands.empty()) return;
  std::vector<u8> blob = EncodeWorldCommands(commands);
  // Reliable, like quests: a dropped spawn or cleanup would desync a client's
  // world from the host's permanently.
  server_.Push(MakePacket(tx::network::ZPeerId::to_all, MessageType::kWorldCommands, blob,
                          /*reliable=*/true, tx::network::PacketPriority::Medium));
}

// --- client ---

ClientSession::ClientSession(SessionConfig config) : config_(std::move(config)) {
  if (config_.snapshot_interval_ticks > 0 && config_.tick_rate > 0) {
    snapshot_dt_ = static_cast<f32>(config_.snapshot_interval_ticks) /
                   static_cast<f32>(config_.tick_rate);
  }
}

ClientSession::~ClientSession() {
  client_.Disconnect();
}

bool ClientSession::Start() {
  const tx::network::ZClient::ConnectionOptions options{
      .use_encryption = false,
      .pre_shared_key = {},
      .use_compression = false,
      .allow_ipv6 = false,
      .start_threads = true,
      .chaos = {}};
  if (!client_.Connect(base::StringRef(config_.address.c_str()), config_.port,
                       options)) {
    REC_ERROR("net: failed to start connecting to {}:{}",
              config_.address.c_str(), config_.port);
    return false;
  }
  REC_INFO("net: connecting to {}:{}", config_.address.c_str(), config_.port);
  return true;
}

void ClientSession::Tick(ecs::World& world, f32 dt) {
  client_.Update();

  using Phase = tx::network::ZClient::HandshakePhase;
  const Phase phase = client_.handshake_phase();
  if (phase == Phase::kFailed) {
    if (!failure_logged_) {
      REC_ERROR("net: handshake failed (reason {})",
                static_cast<u32>(client_.handshake_failure_reason()));
      failure_logged_ = true;
      applier_.Reset(world);
      joined_ = false;
    }
    return;
  }
  if (phase == Phase::kConnected && !join_sent_) {
    ClientJoin join;
    join.protocol = kProtocolVersion;
    join.player_name.assign(config_.player_name.data(),
                            config_.player_name.size());
    client_.Push(MakePacket(tx::network::ZPeerId::to_server,
                            MessageType::kClientJoin, join.Encode(),
                            /*reliable=*/true,
                            tx::network::PacketPriority::High));
    join_sent_ = true;
  }

  PollMessages(world);

  if (joined_) {
    input_.client_tick = tick_;
    client_.Push(MakePacket(tx::network::ZPeerId::to_server,
                            MessageType::kPlayerInput, input_.Encode(),
                            /*reliable=*/false));

    // Heartbeat for headless clients.
    if (tick_ % (static_cast<u64>(config_.tick_rate) * 5) == 0) {
      const ecs::Entity player = player_entity();
      if (const auto* t = player ? world.Get<world::Transform>(player) : nullptr) {
        REC_INFO("net: {} replicated entities, player at ({:.2f}, {:.2f}, {:.2f})",
                 applier_.entity_count(), t->position[0], t->position[1],
                 t->position[2]);
      } else {
        REC_INFO("net: {} replicated entities, player not spawned yet",
                 applier_.entity_count());
      }
    }
  }
  ++tick_;
}

void ClientSession::SendActivate(u64 handle) {
  if (!joined_) return;
  std::vector<u8> payload(8);
  nanobuf::StoreLe<u64>(payload.data(), handle);
  client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kActivateRef, payload,
                          /*reliable=*/true, tx::network::PacketPriority::High));
}

void ClientSession::SendDialogueSelect(u64 info) {
  if (!joined_) return;
  std::vector<u8> payload(8);
  nanobuf::StoreLe<u64>(payload.data(), info);
  client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kDialogueSelect, payload,
                          /*reliable=*/true, tx::network::PacketPriority::High));
}

void ClientSession::SendStageRequest(const StageRequest& req) {
  if (!joined_) return;
  client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kStageRequest,
                          EncodeStageRequest(req), /*reliable=*/true,
                          tx::network::PacketPriority::High));
}

void ClientSession::PollMessages(ecs::World& world) {
  tx::network::IncomingPacket packet;
  while (client_.Poll(tx::network::PacketChannelType::Data, packet)) {
    if (tx::network::IsSystemMessage(packet.type)) continue;
    switch (static_cast<MessageType>(packet.type)) {
      case MessageType::kJoinAccept: {
        auto accept = ParseAs<JoinAcceptView>(packet);
        if (!accept) break;
        joined_ = true;
        player_net_id_ = accept->player_entity();
        if (accept->snapshot_rate() > 0) {
          snapshot_dt_ = 1.0f / static_cast<f32>(accept->snapshot_rate());
        }
        REC_INFO("net: joined as client {} (player entity {})",
                 accept->client_id(), player_net_id_);
        break;
      }
      case MessageType::kJoinRefuse: {
        if (auto refuse = ParseAs<JoinRefuseView>(packet)) {
          const char* reason = refuse->reason().name();
          REC_ERROR("net: server refused join: {}",
                    reason ? reason : "unknown");
        }
        joined_ = false;
        failure_logged_ = true;
        client_.Disconnect();
        return;
      }
      case MessageType::kSnapshot: {
        if (auto snapshot = ParseAs<SnapshotView>(packet)) {
          applier_.Apply(world, *snapshot, snapshot_dt_);
        }
        break;
      }
      case MessageType::kQuestUpdate: {
        if (!quest_sink_) break;
        const ByteSpan blob(reinterpret_cast<const u8*>(packet.data.data()),
                            packet.data.size());
        if (!ApplyQuestUpdate(blob, quest_sink_)) {
          REC_WARN("net: dropped corrupt quest update");
        }
        break;
      }
      case MessageType::kWorldCommands: {
        if (!world_command_sink_) break;
        const ByteSpan blob(reinterpret_cast<const u8*>(packet.data.data()),
                            packet.data.size());
        if (auto cmds = DecodeWorldCommands(blob)) {
          world_command_sink_(*cmds);
        } else {
          REC_WARN("net: dropped corrupt world-command update");
        }
        break;
      }
      case MessageType::kActorSync: {
        if (!actor_sink_) break;
        const ByteSpan blob(reinterpret_cast<const u8*>(packet.data.data()),
                            packet.data.size());
        if (auto actors = DecodeActorStates(blob)) {
          actor_sink_(*actors);
        } else {
          REC_WARN("net: dropped corrupt actor sync");
        }
        break;
      }
      default:
        REC_WARN("net: unhandled message type {}",
                 static_cast<u16>(packet.type));
        break;
    }
  }
}

}  // namespace rec::net
