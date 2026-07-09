#ifndef RECREATION_NET_RPC_CHANNEL_H_
#define RECREATION_NET_RPC_CHANNEL_H_

#include <cstddef>

#include <znet/z_client.h>
#include <znet/z_server.h>

#include "core/types.h"
#include "rpc/rpc_message.h"
#include "rpc/rpc_registry.h"

namespace rx::net {

// Carries scripting RPCs over the session's reliable data channel and dispatches
// inbound calls through a registry the engine populates. The server channel
// attributes each call to the peer that sent it and can target a specific
// client or broadcast; the client channel talks only to the host. Both own their
// registry so the managed runtime registers handlers in one place.

class RpcServerChannel {
 public:
  explicit RpcServerChannel(tx::network::ZServer& server) : server_(server) {}

  rpc::RpcRegistry& registry() { return registry_; }

  // Decodes a kRpcCall from `peer` and dispatches it with that peer as the
  // sender. Drops a malformed call or one with no registered handler.
  void OnPacket(u32 peer, const u8* data, size_t size);

  // Sends a call to one client. Returns false if the encoded call exceeds the
  // single-datagram limit (the caller should split its payload).
  bool EmitToClient(u32 peer, const rpc::RpcCall& call);
  bool EmitToClient(u32 peer, std::string name, rpc::RpcArgs args);

  // Sends a call to every connected client.
  bool Broadcast(const rpc::RpcCall& call);
  bool Broadcast(std::string name, rpc::RpcArgs args);

 private:
  bool Send(u32 destination, const rpc::RpcCall& call);

  tx::network::ZServer& server_;
  rpc::RpcRegistry registry_;
};

class RpcClientChannel {
 public:
  explicit RpcClientChannel(tx::network::ZClient& client) : client_(client) {}

  rpc::RpcRegistry& registry() { return registry_; }

  // Decodes a kRpcCall from the host and dispatches it as a server-originated
  // call. Drops a malformed call or one with no registered handler.
  void OnPacket(const u8* data, size_t size);

  // Sends a call to the host. Returns false if the encoded call exceeds the
  // single-datagram limit.
  bool EmitToServer(const rpc::RpcCall& call);
  bool EmitToServer(std::string name, rpc::RpcArgs args);

 private:
  tx::network::ZClient& client_;
  rpc::RpcRegistry registry_;
};

}  // namespace rx::net

#endif  // RECREATION_NET_RPC_CHANNEL_H_
