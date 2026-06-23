#ifndef RECREATION_NET_PROTOCOL_H_
#define RECREATION_NET_PROTOCOL_H_

#include <optional>
#include <vector>

#include <znet/z_packets.h>
#include <znet/z_peer.h>

#include "protocol.nb.h"

#include "core/types.h"

namespace rec::net {

// Bumped whenever nbuf/protocol.nb or a hand-encoded payload changes shape.
// v2 adds the per-quest domain tag (multi-game quest replication).
// v3 adds asset streaming (manifest/request) and the scripting RPC channel.
inline constexpr u32 kProtocolVersion = 3;

// Application packet ids. Zetanet owns everything below PacketType::Message
// (100), recreation ids start at 101. Every payload is a nanobuf message
// from nbuf/protocol.nb; nothing crosses the wire outside those schemas.
enum class MessageType : u16 {
  kClientJoin = 101,
  kJoinAccept = 102,
  kJoinRefuse = 103,
  kSnapshot = 104,
  kPlayerInput = 105,
  kQuestUpdate = 106,
  kWorldCommands = 107,  // quest-driven world mutations (spawn/move/enable/cleanup)
  kActivateRef = 108,    // client -> server: the player activated a reference
  kActorSync = 109,      // server -> clients: NPC transforms that changed
  kDialogueSelect = 110,  // client -> server: the player chose a dialogue INFO
  kStageRequest = 111,    // client -> server: a debugger stage/objective/running change
  kObjectiveMarker = 112,  // server -> clients: the active quest objective waypoint
  kAssetManifest = 113,   // server -> client: the mod manifest offered for streaming
  kAssetRequest = 114,    // client -> server: content hashes the client wants streamed
  kRpcCall = 115,         // either direction: an encoded scripting RPC (rpc::EncodeCall)
};

inline tx::network::PacketType ToPacketType(MessageType type) {
  return static_cast<tx::network::PacketType>(type);
}

// Wraps an encoded nanobuf payload in an outgoing zetanet packet. Snapshots
// stay unreliable by design, joins and keyframes ride the reliable path.
inline tx::network::OutgoingPacket MakePacket(
    u32 destination, MessageType type, const std::vector<u8>& payload,
    bool reliable,
    tx::network::PacketPriority priority = tx::network::PacketPriority::Medium) {
  const tx::network::PackageFlags flags{
      .reliable = reliable ? u8{1} : u8{0},
      .encrypted = 0,
      .compressed = 0,
      .priority = static_cast<u8>(priority),
      .acknowledged = 0,
      .awaiting_ack = reliable ? u8{1} : u8{0},
      .reserved = 0,
  };
  return tx::network::OutgoingPacket(
      destination, ToPacketType(type), tx::network::PacketChannelType::Data,
      flags,
      base::Span<byte>(reinterpret_cast<const byte*>(payload.data()),
                       payload.size()));
}

// Parses an incoming packet's payload as a nanobuf view. Returns nullopt on
// a corrupt buffer; the caller drops the packet.
template <typename View>
std::optional<View> ParseAs(const tx::network::IncomingPacket& packet) {
  return View::Parse(reinterpret_cast<const u8*>(packet.data.data()),
                     packet.data.size());
}

}  // namespace rec::net

#endif  // RECREATION_NET_PROTOCOL_H_
