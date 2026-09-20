// item_sync_nettest: loot as one world's loot. The item itself replicates like
// anything else the host owns, so what travels here is the one thing a snapshot
// cannot carry: which replica is loot, and the base record to render it from.
// This covers the codec, the announcement reaching a client, a joining client
// being caught up on what is already on the floor, and an item the host says is
// gone dropping out of that catch-up.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here, matching the other net tests.

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "components/gamenet/item_sync.h"
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

struct Receipt {
  int count = 0;
  net::WorldItemState last;
};

}  // namespace

int main() {
  std::printf("item_sync_nettest\n");

  // --- codec ---
  {
    const net::WorldItemState item{0x0102030405060708ull, 0x0000000100012E46ull};
    const std::vector<rx::u8> encoded = net::EncodeWorldItem(item);
    Check("the record is 16 bytes", encoded.size() == 16);
    const auto decoded = net::DecodeWorldItem(encoded.data(), encoded.size());
    Check("it round-trips",
          decoded && decoded->net_id == item.net_id && decoded->base == item.base);
    Check("a wrong-size buffer is refused",
          !net::DecodeWorldItem(encoded.data(), 15) &&
              !net::DecodeWorldItem(encoded.data(), 17) && !net::DecodeWorldItem(nullptr, 16));
  }

  // --- wire ---
  net::GameSessionConfig server_cfg;
  server_cfg.port = 29769;
  server_cfg.client_timeout_seconds = 1.0f;
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29769;
  client_cfg.address = base::String("127.0.0.1");
  auto client1 = std::make_unique<net::GameClientSession>(client_cfg);
  Check("client starts", client1->Start());

  ecs::World sworld, w1, w2;
  Receipt r1;
  client1->SetWorldItemSink([&](const net::WorldItemState& item) {
    ++r1.count;
    r1.last = item;
  });

  bool joined = false;
  for (int i = 0; i < 2000 && !joined; ++i) {
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
    joined = client1->joined();
  }
  Check("first client joined", joined);

  // Something hits the floor.
  server.SendWorldItem({0xABCDull, 0x0000000100012E46ull});
  for (int i = 0; i < 600 && r1.count == 0; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);
  Check("the announcement arrived", r1.count > 0);
  Check("it names the replica and the record",
        r1.last.net_id == 0xABCDull && r1.last.base == 0x0000000100012E46ull);

  // A second piece, so the catch-up below has more than one thing to say.
  server.SendWorldItem({0xBEEFull, 0x0000000100099999ull});
  for (int i = 0; i < 120; ++i)
    Pump(server, sworld, client1.get(), w1, nullptr, w2);

  // Somebody picks the first one up: the host stops telling joiners about it.
  server.ForgetWorldItem(0xABCDull);

  // A client that connects only now is caught up on what is still lying there,
  // and not on what is gone. It is built here rather than up front on purpose: a
  // client that is merely transport-connected already receives broadcasts, so one
  // created earlier would hear the live announcements too and this would not be
  // testing the catch-up at all. (A client that hears both is fine in the engine,
  // where showing an item its mesh is idempotent.)
  auto client2 = std::make_unique<net::GameClientSession>(client_cfg);
  Check("the late client starts", client2->Start());
  Receipt r2;
  client2->SetWorldItemSink([&](const net::WorldItemState& item) {
    ++r2.count;
    r2.last = item;
  });
  bool joined2 = false;
  for (int i = 0; i < 2000 && !joined2; ++i) {
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
    joined2 = client2->joined();
  }
  Check("second client joined", joined2);
  for (int i = 0; i < 600 && r2.count == 0; ++i)
    Pump(server, sworld, client1.get(), w1, client2.get(), w2);
  Check("the late joiner is told about the loot still on the floor", r2.count == 1);
  Check("and told about the piece that is still there, not the one taken",
        r2.last.net_id == 0xBEEFull);

  std::printf("item_sync_nettest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
