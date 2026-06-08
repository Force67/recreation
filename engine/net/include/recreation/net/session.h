#ifndef RECREATION_NET_SESSION_H_
#define RECREATION_NET_SESSION_H_

#include <memory>
#include <vector>

#include "recreation/ecs/world.h"
#include "recreation/net/replication.h"
#include "recreation/net/transport.h"

namespace rec::net {

// The server simulates, clients predict locally and reconcile. One process
// can run both (listen server) since sessions only touch the world through
// the replication registry.
class Session {
 public:
  virtual ~Session() = default;
  virtual void Tick(ecs::World& world, f32 dt) = 0;
};

class ServerSession final : public Session {
 public:
  ServerSession(u16 port, ReplicationRegistry& registry);

  void Tick(ecs::World& world, f32 dt) override;
  size_t client_count() const { return clients_.size(); }

 private:
  struct Client {
    Endpoint endpoint;
    u16 last_acked_sequence = 0;
  };

  UdpSocket socket_;
  ReplicationRegistry& registry_;
  std::vector<Client> clients_;
  u16 sequence_ = 0;
};

class ClientSession final : public Session {
 public:
  ClientSession(Endpoint server, ReplicationRegistry& registry);

  void Tick(ecs::World& world, f32 dt) override;
  bool connected() const { return connected_; }

 private:
  UdpSocket socket_;
  Endpoint server_;
  ReplicationRegistry& registry_;
  bool connected_ = false;
};

}  // namespace rec::net

#endif  // RECREATION_NET_SESSION_H_
