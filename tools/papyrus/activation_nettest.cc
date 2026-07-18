// activation_nettest: activation requests retain their admitted peer identity,
// reject malformed payloads, and are bounded per peer before reaching gameplay.

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "ecs/world.h"
#include "gamenet/protocol.h"
#include "gamenet/session.h"

namespace {

int failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++failures;
}

void Pump(rx::net::GameServerSession& server, rx::ecs::World& server_world,
          rx::net::GameClientSession& client, rx::ecs::World& client_world) {
  server.Tick(server_world, 1.0f / 60.0f);
  client.Tick(client_world, 1.0f / 60.0f);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
}

}  // namespace

int main() {
  std::puts("activation_nettest");

  rx::net::GameSessionConfig server_config;
  server_config.port = 29754;
  // Keep the fixed-window open for the whole test while still exercising the
  // configured-tick-rate calculation used by production sessions.
  server_config.tick_rate = 10000;
  rx::net::GameServerSession server(server_config);
  rx::u32 observed_peer = 0;
  rx::u64 observed_handle = 0;
  rx::u32 accepted = 0;
  bool observed = false;
  bool peer_changed = false;
  server.SetActivateSink([&](rx::u32 peer, rx::u64 handle) {
    if (observed && observed_peer != peer) peer_changed = true;
    observed = true;
    observed_peer = peer;
    observed_handle = handle;
    ++accepted;
  });
  Check("server starts", server.Start());

  rx::net::GameSessionConfig client_config;
  client_config.port = server_config.port;
  client_config.address = base::String("127.0.0.1");
  client_config.player_name = base::NameString("activator");
  rx::net::GameClientSession client(client_config);
  Check("client starts", client.Start());

  rx::ecs::World server_world;
  rx::ecs::World client_world;
  for (int i = 0; i < 600 && !client.joined(); ++i)
    Pump(server, server_world, client, client_world);
  Check("client joins", client.joined());

  constexpr rx::u64 kHandle = 0x0000000100012345ull;
  client.SendActivate(kHandle);
  for (int i = 0; i < 120 && accepted == 0; ++i)
    Pump(server, server_world, client, client_world);
  Check("activation retains its peer context",
        accepted == 1 && observed && !peer_changed &&
            server_world.IsAlive(server.engine().PlayerOf(observed_peer)));
  Check("activation preserves its handle", observed_handle == kHandle);

  const rx::u32 before_malformed = accepted;
  std::vector<rx::u8> short_payload(7, 0);
  std::vector<rx::u8> long_payload(9, 0);
  client.engine().SendToServer(static_cast<rx::u16>(rx::net::GameMessage::kActivateRef),
                               short_payload, true);
  client.engine().SendToServer(static_cast<rx::u16>(rx::net::GameMessage::kActivateRef),
                               long_payload, true);
  client.SendActivate(0);
  for (int i = 0; i < 90; ++i) Pump(server, server_world, client, client_world);
  Check("malformed and zero-handle requests are rejected", accepted == before_malformed);

  for (rx::u32 i = 0; i < rx::net::GameServerSession::kMaxActivationRequestsPerSecond + 8; ++i)
    client.SendActivate(kHandle + i + 1);
  for (int i = 0; i < 300; ++i) Pump(server, server_world, client, client_world);
  Check("per-peer activation rate is bounded",
        accepted == rx::net::GameServerSession::kMaxActivationRequestsPerSecond);
  Check("rate accounting remains scoped to the admitted peer", !peer_changed);

  if (failures == 0) {
    std::puts("activation net: all checks passed");
    return 0;
  }
  std::printf("activation net: %d checks FAILED\n", failures);
  return 1;
}
