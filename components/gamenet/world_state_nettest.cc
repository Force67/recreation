// world_state_nettest: loopback check of the shared clock and sky. The host
// samples its world state every tick and kWorldState carries only what a client
// cannot work out for itself, so this verifies the codec round-trips, that the
// first sample reaches a client, that ordinary passing time does NOT (the client
// extrapolates it), that a clock jump and a changed weather seed do, that the
// heartbeat eventually re-states the world, and that a client joining later is
// told the time as it is admitted.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here, matching the other net tests.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "components/gamenet/session.h"
#include "components/gamenet/world_state.h"
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

struct Receipt {
  int count = 0;
  net::WorldState state;
};

}  // namespace

int main() {
  std::printf("world_state_nettest\n");

  // --- codec round-trip ---
  {
    const net::WorldState state{1234.567890123, 20.0f, 0xfeedfacecafebeefull,
                               0x0000000300012345ull};
    const std::vector<rx::u8> encoded = net::EncodeWorldState(state);
    Check("the record is 28 bytes", encoded.size() == 28);
    const auto decoded = net::DecodeWorldState(encoded.data(), encoded.size());
    Check("world state round-trips",
          decoded && decoded->game_days == state.game_days &&
              decoded->timescale == state.timescale &&
              decoded->weather_seed == state.weather_seed && decoded->weather == state.weather);
    Check("a wrong-size buffer is refused",
          !net::DecodeWorldState(encoded.data(), 27) &&
              !net::DecodeWorldState(encoded.data(), 29) && !net::DecodeWorldState(nullptr, 28));
  }

  // --- wire ---
  net::GameSessionConfig server_cfg;
  server_cfg.port = 29761;
  server_cfg.client_timeout_seconds = 1.0f;
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  // The host's world, driven by hand: the source is sampled every server tick,
  // exactly as the engine's clock and weather director are.
  net::WorldState host{10.5, 20.0f, 0xBEE71Eull, 0x11ull};
  server.SetWorldStateSource([&]() { return host; });

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29761;
  client_cfg.address = base::String("127.0.0.1");
  auto client1 = std::make_unique<net::GameClientSession>(client_cfg);
  auto client2 = std::make_unique<net::GameClientSession>(client_cfg);
  Check("clients start", client1->Start() && client2->Start());

  ecs::World sworld, w1, w2;
  Receipt r1;
  // The sink goes up before the join: the admission message can arrive in the
  // very pumps that notice joined().
  client1->SetWorldStateSink([&](const net::WorldState& s) {
    ++r1.count;
    r1.state = s;
  });

  bool joined = false;
  for (int i = 0; i < 2000 && !joined; ++i) {
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
    joined = client1->joined();
  }
  Check("first client joined", joined);
  for (int i = 0; i < 600 && r1.count == 0; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  Check("the client was told the world state", r1.count > 0);
  Check("the values survived the wire",
        r1.state.game_days == host.game_days && r1.state.timescale == host.timescale &&
            r1.state.weather_seed == host.weather_seed && r1.state.weather == host.weather);

  // Time passing at the timescale the client was told is not news: it runs its
  // own clock forward. 60 pumps is one second of server time, well inside the
  // heartbeat.
  r1.count = 0;
  for (int i = 0; i < 60; ++i) {
    host.game_days += (1.0 / 60.0) * host.timescale / 86400.0;
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  }
  Check("ordinary passing time stays off the wire", r1.count == 0);

  // A clock the host moved (a script setting the hour) is news.
  r1.count = 0;
  host.game_days += 0.25;  // six hours
  for (int i = 0; i < 600 && r1.count == 0; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  Check("a clock jump is announced", r1.count > 0);
  Check("the jumped time arrived", std::abs(r1.state.game_days - host.game_days) < 1e-6);

  // So is a different sky.
  r1.count = 0;
  host.weather_seed = 0x5EEDull;
  host.weather = 0x22ull;
  for (int i = 0; i < 600 && r1.count == 0; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  Check("a changed weather seed is announced", r1.count > 0);
  Check("the new sky arrived",
        r1.state.weather_seed == 0x5EEDull && r1.state.weather == 0x22ull);

  // And the heartbeat re-states the world even when nothing changed, so a client
  // that missed a message is not left in yesterday. Five seconds of server time.
  r1.count = 0;
  for (int i = 0; i < 400 && r1.count == 0; ++i) {
    host.game_days += (1.0 / 60.0) * host.timescale / 86400.0;
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  }
  Check("the heartbeat re-states the world", r1.count > 0);

  // A later joiner is told the time as it is admitted, not on the next beat.
  Receipt r2;
  client2->SetWorldStateSink([&](const net::WorldState& s) {
    ++r2.count;
    r2.state = s;
  });
  bool joined2 = false;
  for (int i = 0; i < 2000 && !joined2; ++i) {
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
    joined2 = client2->joined();
  }
  Check("second client joined", joined2);
  for (int i = 0; i < 600 && r2.count == 0; ++i)
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
  Check("the late joiner was told the world state", r2.count > 0);
  Check("the late joiner got the current sky",
        r2.state.weather_seed == host.weather_seed && r2.state.weather == host.weather);

  std::printf("world_state_nettest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
