// player_attack_nettest: the swing request. A client cannot be trusted to say
// what it hit, so all it sends is the aim, and the host decides the rest. This
// covers the codec (including the NaN a hostile client could send to poison
// every arc test downstream), that a swing reaches the host with its aim intact,
// and the cadence: the host accepts one swing per melee interval and drops the
// rest, which is the rate limit and the game rule at once.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here, matching the other net tests.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "components/gamenet/player_sync.h"
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
          net::GameClientSession* client,
          ecs::World& cworld,
          float dt = 1.0f / 60.0f) {
  server.Tick(sworld, dt);
  if (client)
    client->Tick(cworld, dt);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

}  // namespace

int main() {
  std::printf("player_attack_nettest\n");

  // --- codec ---
  {
    const std::vector<rx::u8> encoded = net::EncodePlayerAttack(1.75f);
    Check("the record is 4 bytes", encoded.size() == 4);
    const auto decoded = net::DecodePlayerAttack(encoded.data(), encoded.size());
    Check("the aim round-trips", decoded && *decoded == 1.75f);
    Check("a negative aim round-trips",
          net::DecodePlayerAttack(net::EncodePlayerAttack(-2.5f).data(), 4).value_or(0.0f) ==
              -2.5f);
    Check("a wrong-size buffer is refused",
          !net::DecodePlayerAttack(encoded.data(), 3) &&
              !net::DecodePlayerAttack(encoded.data(), 5) && !net::DecodePlayerAttack(nullptr, 4));
    // A NaN would pass every comparison in the arc test and make the swing hit
    // nothing, forever; refuse it at the door instead.
    const std::vector<rx::u8> nan_aim = net::EncodePlayerAttack(std::nanf(""));
    Check("a NaN aim is refused", !net::DecodePlayerAttack(nan_aim.data(), nan_aim.size()));
    const std::vector<rx::u8> inf_aim = net::EncodePlayerAttack(HUGE_VALF);
    Check("an infinite aim is refused", !net::DecodePlayerAttack(inf_aim.data(), inf_aim.size()));
  }

  // --- wire ---
  net::GameSessionConfig server_cfg;
  server_cfg.port = 29767;
  server_cfg.client_timeout_seconds = 1.0f;
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  int swings = 0;
  float last_aim = 0.0f;
  rx::u32 last_peer = 0xffffffffu;
  server.SetPlayerAttackSink([&](rx::u32 peer, float yaw) {
    ++swings;
    last_aim = yaw;
    last_peer = peer;
  });
  server.set_swing_cadence_seconds(1.0f);

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29767;
  client_cfg.address = base::String("127.0.0.1");
  auto client = std::make_unique<net::GameClientSession>(client_cfg);
  Check("client starts", client->Start());

  ecs::World sworld, cworld;
  bool joined = false;
  for (int i = 0; i < 2000 && !joined; ++i) {
    Pump(server, sworld, client.get(), cworld);
    joined = client->joined();
  }
  Check("client joined", joined);

  client->SendAttack(0.75f);
  for (int i = 0; i < 600 && swings == 0; ++i)
    Pump(server, sworld, client.get(), cworld);
  Check("the swing reached the host", swings == 1);
  Check("the aim survived the wire", last_aim == 0.75f);
  Check("the host knows who swung", last_peer == 0 || last_peer != 0xffffffffu);

  // Spamming inside the cadence buys nothing.
  for (int i = 0; i < 20; ++i)
    client->SendAttack(0.1f);
  for (int i = 0; i < 120; ++i)
    Pump(server, sworld, client.get(), cworld);
  Check("swings inside the cadence are dropped", swings == 1);

  // Once the cadence has passed, the next one is accepted. The server's clock
  // advances by the dt it is ticked with, so tick it forward a second.
  for (int i = 0; i < 12; ++i)
    Pump(server, sworld, client.get(), cworld, 0.1f);
  client->SendAttack(-1.25f);
  for (int i = 0; i < 600 && swings == 1; ++i)
    Pump(server, sworld, client.get(), cworld);
  Check("a swing after the cadence is accepted", swings == 2);
  Check("and carries its own aim", last_aim == -1.25f);

  std::printf("player_attack_nettest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
