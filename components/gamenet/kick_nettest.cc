// kick_nettest: dropping a player for real. The privileged commands are managed
// code, so the engine has to be able to actually disconnect somebody on their
// behalf -- a kick that reports success and drops nobody is worse than no kick.
// This checks a kicked client leaves (over loopback, two real sessions), that
// the other client stays, and that kicking somebody who is not connected is
// refused rather than quietly claimed.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here, matching the other net tests.

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "components/gamenet/session.h"
#include "core/types.h"
#include "ecs/world.h"

namespace net = rx::net;
namespace ecs = rx::ecs;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void Pump(net::GameServerSession& server,
          ecs::World& sworld,
          net::GameClientSession* c1,
          ecs::World& w1,
          net::GameClientSession* c2,
          ecs::World& w2) {
  const float dt = 1.0f / 60.0f;
  server.Tick(sworld, dt);
  if (c1)
    c1->Tick(w1, dt);
  if (c2)
    c2->Tick(w2, dt);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

}  // namespace

int main() {
  std::printf("kick_nettest\n");

  net::GameSessionConfig server_cfg;
  server_cfg.port = 29763;
  server_cfg.client_timeout_seconds = 1.0f;
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  Check("kicking a peer nobody knows is refused", !server.Kick(0));
  Check("kicking a made-up peer is refused", !server.Kick(4242));

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29763;
  client_cfg.address = base::String("127.0.0.1");
  auto client1 = std::make_unique<net::GameClientSession>(client_cfg);
  auto client2 = std::make_unique<net::GameClientSession>(client_cfg);
  Check("clients start", client1->Start() && client2->Start());

  ecs::World sworld, w1, w2;
  bool both = false;
  for (int i = 0; i < 4000 && !both; ++i) {
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
    both = client1->joined() && client2->joined();
  }
  Check("both clients joined", both);

  // Find the peer the first client is, so the kick names somebody real.
  rx::u32 victim = 0;
  bool found = false;
  server.engine().ForEachPeer([&](rx::u32 peer) {
    if (!found) {
      victim = peer;
      found = true;
    }
  });
  Check("the server knows its peers", found);

  Check("kicking a joined peer is accepted", server.Kick(victim));

  // One of the two is gone; which one depends on which peer id came first.
  bool one_left = false;
  for (int i = 0; i < 600 && !one_left; ++i) {
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
    one_left = !client1->joined() || !client2->joined();
  }
  Check("the kicked client left", one_left);
  Check("the other client stayed", client1->joined() || client2->joined());

  // The server's own roster clears on the timeout that follows, and the peer is
  // then as unknown as any other stranger.
  bool forgotten = false;
  for (int i = 0; i < 4000 && !forgotten; ++i) {
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
    forgotten = !server.Kick(victim);
  }
  Check("the server forgets the kicked peer", forgotten);

  std::printf("kick_nettest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
