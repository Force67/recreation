// player_sync_nettest: loopback check of the player-presence messages. Avatar
// announcements and replicated vitals ride kPlayerAvatar / kPlayerState over
// the reliable channel; the test verifies the codecs round-trip, that a
// broadcast reaches every connected client, that a client joining later
// receives the current vitals table, that unchanged values stay off the wire,
// and that a departed player's entry leaves the join-time table.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here, matching the other net tests.

#include <chrono>
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

struct VitalsReceipt {
  bool got = false;
  rx::u64 net_id = 0;
  rx::u16 health = 0;
  rx::u16 max = 0;
  bool dead = false;
};

}  // namespace

int main() {
  std::printf("player_sync_nettest\n");

  // --- codec round-trips ---
  {
    const net::PlayerAvatarState avatar{0x0102030405060708ull, 0x0000000700000014ull};
    const std::vector<rx::u8> encoded = net::EncodePlayerAvatar(avatar);
    const auto decoded = net::DecodePlayerAvatar(encoded.data(), encoded.size());
    Check("avatar round-trips",
          decoded && decoded->net_id == avatar.net_id && decoded->form == avatar.form);
    Check("avatar rejects a wrong-size buffer",
          !net::DecodePlayerAvatar(encoded.data(), 15) && !net::DecodePlayerAvatar(nullptr, 0));

    const net::PlayerVitals vitals{42, 42, 100, true};
    const std::vector<rx::u8> encoded_v = net::EncodePlayerVitals(vitals);
    const auto decoded_v = net::DecodePlayerVitals(encoded_v.data(), encoded_v.size());
    Check("vitals round-trip", decoded_v && decoded_v->net_id == 42 &&
                                   decoded_v->health == 42 && decoded_v->max_health == 100 &&
                                   decoded_v->dead);
    const net::PlayerVitals alive{300, 300, false};
    const auto decoded_a = net::DecodePlayerVitals(net::EncodePlayerVitals(alive).data(), 13);
    Check("vitals keep an alive flag",
          decoded_a && !decoded_a->dead && decoded_a->health == 300);
    Check("vitals reject a wrong-size buffer", !net::DecodePlayerVitals(encoded_v.data(), 12));
  }

  // --- wire: broadcasts, the join-time table, and departure cleanup ---
  net::GameSessionConfig server_cfg;
  server_cfg.port = 29759;
  server_cfg.client_timeout_seconds = 1.0f;
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29759;
  client_cfg.address = base::String("127.0.0.1");
  auto client1 = std::make_unique<net::GameClientSession>(client_cfg);
  auto client2 = std::make_unique<net::GameClientSession>(client_cfg);
  Check("clients start", client1->Start() && client2->Start());

  ecs::World sworld, w1, w2;
  bool c1_joined = false;
  for (int i = 0; i < 2000 && !c1_joined; ++i) {
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
    c1_joined = client1->joined();
  }
  Check("first client joined", c1_joined);

  // An avatar announcement reaches every connected client with its values.
  struct AvatarReceipt {
    bool got = false;
    rx::u64 net_id = 0;
    rx::u64 form = 0;
  };
  AvatarReceipt a1;
  client1->SetPlayerAvatarSink([&](rx::u64 id, rx::u64 form) { a1 = {true, id, form}; });
  server.engine().Broadcast(static_cast<rx::u16>(net::GameMessage::kPlayerAvatar),
                            net::EncodePlayerAvatar(net::PlayerAvatarState{77, 0x7ull}),
                            /*reliable=*/true, tx::network::PacketPriority::Medium);
  for (int i = 0; i < 600 && !a1.got; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  Check("avatar announcement arrived", a1.got);
  Check("avatar values survived the wire", a1.net_id == 77 && a1.form == 0x7ull);

  // The host announces a player's vitals; connected clients hear it.
  VitalsReceipt r1, r2;
  client1->SetPlayerVitalsSink(
      [&](rx::u64 id, rx::u16 h, rx::u16 m, bool d) { r1 = {true, id, h, m, d}; });
  server.SetPlayerHealth(0, 55, 100, false);
  for (int i = 0; i < 600 && !r1.got; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  Check("client received the vitals broadcast", r1.got);
  Check("vitals values survived the wire",
        r1.net_id != 0 && r1.health == 55 && r1.max == 100 && !r1.dead);
  Check("the broadcast resolves to the player's net id", r1.net_id == server.PlayerNetId(0));

  // A client joining after the announcement catches up through the join-time
  // vitals table.
  // The sink goes up before the join: the join-time table can arrive in the
  // very pumps that notice joined().
  r2 = VitalsReceipt{};
  client2->SetPlayerVitalsSink(
      [&](rx::u64 id, rx::u16 h, rx::u16 m, bool d) { r2 = {true, id, h, m, d}; });
  bool c2_joined = false;
  for (int i = 0; i < 2000 && !c2_joined; ++i) {
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
    c2_joined = client2->joined();
  }
  Check("second client joined", c2_joined);
  for (int i = 0; i < 600 && !r2.got; ++i)
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
  Check("late joiner received the vitals table", r2.got);
  Check("the table entry matches the announcement",
        r2.health == 55 && r2.max == 100 && !r2.dead && r2.net_id == server.PlayerNetId(0));

  // An unchanged announcement stays quiet; a change goes out again.
  r1 = VitalsReceipt{};
  server.SetPlayerHealth(0, 55, 100, false);
  for (int i = 0; i < 120 && !r1.got; ++i)
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
  Check("an unchanged value is not re-sent", !r1.got);
  server.SetPlayerHealth(0, 0, 100, true);
  for (int i = 0; i < 600 && !r1.got; ++i)
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
  Check("a change re-announces", r1.got && r1.dead && r1.health == 0);

  // The departed player's entry leaves the join-time table (and a later joiner
  // is not told about a ghost).
  bool server_saw_left = false;
  server.SetClientLeftSink([&](rx::u32) { server_saw_left = true; });
  client1.reset();  // drop the connection; the server times the peer out
  for (int i = 0; i < 2000 && !server_saw_left; ++i)
    Pump(server, sworld, nullptr, w1, client2.get(), w2);
  Check("server noticed the departure", server_saw_left);
  Check("the departed player's net id is gone", server.PlayerNetId(0) == 0);
  r2 = VitalsReceipt{};
  server.SetPlayerHealth(0, 10, 100, false);  // a departed peer: no broadcast
  for (int i = 0; i < 120 && !r2.got; ++i)
    Pump(server, sworld, nullptr, w1, client2.get(), w2);
  Check("a departed player's vitals are not announced", !r2.got);

  std::printf("player_sync_nettest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
