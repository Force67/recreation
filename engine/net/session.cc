#include "net/session.h"

#include "core/log.h"

namespace rec::net {

ServerSession::ServerSession(u16 port, ReplicationRegistry& registry) : registry_(registry) {
  if (socket_.Bind(port)) REC_INFO("server listening on {}", port);
}

void ServerSession::Tick(ecs::World& world, f32 dt) {
  Endpoint from;
  while (auto packet = socket_.Receive(&from)) {
    // TODO: handshake channel adds clients, input channel feeds the sim,
    // ack header trims per client delta baselines.
  }

  std::vector<u8> snapshot;
  registry_.WriteSnapshot(world, &snapshot);
  for (const Client& client : clients_) {
    socket_.Send(client.endpoint, ByteSpan(snapshot));
  }
  ++sequence_;
}

ClientSession::ClientSession(Endpoint server, ReplicationRegistry& registry)
    : server_(std::move(server)), registry_(registry) {}

void ClientSession::Tick(ecs::World& world, f32 dt) {
  // TODO: send connect request until acked, then input each tick. Predicted
  // components roll back and replay on snapshot receipt.
  while (auto packet = socket_.Receive(nullptr)) {
    connected_ = true;
    registry_.ApplySnapshot(world, ByteSpan(*packet));
  }
}

}  // namespace rec::net
