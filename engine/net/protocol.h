#ifndef RECREATION_NET_PROTOCOL_H_
#define RECREATION_NET_PROTOCOL_H_

#include <optional>
#include <vector>

#include <znet/z_packets.h>
#include <znet/z_peer.h>

#include "protocol.nb.h"

#include "core/types.h"

namespace rec::net {

// Bumped whenever nbuf/protocol.nb changes shape.
inline constexpr u32 kProtocolVersion = 1;

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
