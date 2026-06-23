#include "net/rpc_channel.h"

#include <utility>

#include "core/log.h"
#include "net/protocol.h"

namespace rec::net {
namespace {

// An encoded call must fit in one datagram (no fragmentation). A call larger
// than this is refused rather than silently dropped on the wire.
constexpr size_t kMaxRpcPacket = 60000;

}  // namespace

// --- server ---

void RpcServerChannel::OnPacket(u32 peer, const u8* data, size_t size) {
  std::optional<rpc::RpcCall> call = rpc::DecodeCall(data, size);
  if (!call) {
    REC_WARN("net: dropped corrupt rpc from peer {}", peer);
    return;
  }
  const rpc::RpcContext ctx{peer, /*from_server=*/false};
  if (!registry_.Dispatch(ctx, *call)) {
    REC_WARN("net: no rpc handler '{}' (from peer {})", call->name, peer);
  }
}

bool RpcServerChannel::Send(u32 destination, const rpc::RpcCall& call) {
  std::vector<u8> payload = rpc::EncodeCall(call);
  if (payload.size() > kMaxRpcPacket) {
    REC_WARN("net: rpc '{}' is too large to send ({} bytes)", call.name, payload.size());
    return false;
  }
  server_.Push(MakePacket(destination, MessageType::kRpcCall, payload,
                          /*reliable=*/true, tx::network::PacketPriority::Medium));
  return true;
}

bool RpcServerChannel::EmitToClient(u32 peer, const rpc::RpcCall& call) {
  return Send(peer, call);
}

bool RpcServerChannel::EmitToClient(u32 peer, std::string name, rpc::RpcArgs args) {
  return Send(peer, rpc::RpcCall{std::move(name), std::move(args)});
}

bool RpcServerChannel::Broadcast(const rpc::RpcCall& call) {
  return Send(tx::network::ZPeerId::to_all, call);
}

bool RpcServerChannel::Broadcast(std::string name, rpc::RpcArgs args) {
  return Send(tx::network::ZPeerId::to_all,
              rpc::RpcCall{std::move(name), std::move(args)});
}

// --- client ---

void RpcClientChannel::OnPacket(const u8* data, size_t size) {
  std::optional<rpc::RpcCall> call = rpc::DecodeCall(data, size);
  if (!call) {
    REC_WARN("net: dropped corrupt rpc from server");
    return;
  }
  const rpc::RpcContext ctx{0, /*from_server=*/true};
  if (!registry_.Dispatch(ctx, *call)) {
    REC_WARN("net: no rpc handler '{}' (from server)", call->name);
  }
}

bool RpcClientChannel::EmitToServer(const rpc::RpcCall& call) {
  std::vector<u8> payload = rpc::EncodeCall(call);
  if (payload.size() > kMaxRpcPacket) {
    REC_WARN("net: rpc '{}' is too large to send ({} bytes)", call.name, payload.size());
    return false;
  }
  client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kRpcCall,
                          payload, /*reliable=*/true, tx::network::PacketPriority::Medium));
  return true;
}

bool RpcClientChannel::EmitToServer(std::string name, rpc::RpcArgs args) {
  return EmitToServer(rpc::RpcCall{std::move(name), std::move(args)});
}

}  // namespace rec::net
