#ifndef RECREATION_NET_TRANSPORT_H_
#define RECREATION_NET_TRANSPORT_H_

#include <optional>
#include <string>
#include <vector>

#include "recreation/core/types.h"

namespace rec::net {

struct Endpoint {
  std::string address;
  u16 port = 0;
};

// Unreliable datagram transport. Reliability where needed (connection
// handshake, chat, inventory ops) is layered per channel on top, state
// snapshots stay unreliable by design.
class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool Bind(u16 port);
  bool Send(const Endpoint& to, ByteSpan data);
  std::optional<std::vector<u8>> Receive(Endpoint* from);

 private:
  i64 handle_ = -1;
};

struct PacketHeader {
  u16 sequence = 0;
  u16 ack = 0;
  u32 ack_bits = 0;  // sliding window of received sequences
  u8 channel = 0;
};

}  // namespace rec::net

#endif  // RECREATION_NET_TRANSPORT_H_
