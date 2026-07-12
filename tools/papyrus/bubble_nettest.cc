// bubble_nettest: streaming bubbles end-to-end over a loopback session. Two
// clients join a server running with a 10-unit bubble radius; the server
// spawns replicated props near and far from the players. Each client must
// receive only its bubble's slice of the world, entities must spawn as they
// enter a bubble and despawn as they leave, every client must mirror the
// session's bubbles (kBubbleSync), overlapping bubbles must agree on exactly
// one owner, and the send stats must show the bandwidth cut against full
// visibility. No game data, so it runs in the ctest gate.

#include <chrono>
#include <cstdio>
#include <thread>

#include "ecs/world.h"
#include "gamenet/session.h"
#include "net/replication.h"
#include "scene/components.h"

namespace net = rx::net;
namespace ecs = rx::ecs;
namespace scene = rx::scene;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Pumps all three sessions one fixed step and yields briefly so the threaded
// transport makes progress.
void Pump(net::GameServerSession& server, ecs::World& sworld,
          net::GameClientSession& c1, ecs::World& w1,
          net::GameClientSession& c2, ecs::World& w2) {
  const float dt = 1.0f / 60.0f;
  server.Tick(sworld, dt);
  c1.Tick(w1, dt);
  c2.Tick(w2, dt);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
}

ecs::Entity SpawnProp(ecs::World& world, float x, float z, rx::u64* net_id) {
  ecs::Entity e = world.Create();
  scene::Transform t;
  t.position[0] = x;
  t.position[2] = z;
  world.Add(e, t);
  const net::NetworkId id = net::AllocateNetworkId();
  *net_id = id.value;
  world.Add(e, id);
  return e;
}

}  // namespace

int main() {
  std::printf("bubble_nettest\n");

  net::GameSessionConfig server_cfg;
  server_cfg.port = 29753;
  server_cfg.bubble_radius = 10.0f;
  server_cfg.keyframe_interval_ticks = 30;
  net::GameServerSession server(server_cfg);
  Check("server starts (bubbles on)", server.Start());

  net::GameSessionConfig c1_cfg;
  c1_cfg.port = 29753;
  c1_cfg.address = base::String("127.0.0.1");
  c1_cfg.player_name = base::NameString("near");
  net::GameClientSession c1(c1_cfg);
  Check("client 1 starts", c1.Start());

  net::GameSessionConfig c2_cfg;
  c2_cfg.port = 29753;
  c2_cfg.address = base::String("127.0.0.1");
  c2_cfg.player_name = base::NameString("also-near");
  net::GameClientSession c2(c2_cfg);
  Check("client 2 starts", c2.Start());

  ecs::World sworld;
  ecs::World w1;
  ecs::World w2;

  for (int i = 0; i < 600 && !(c1.joined() && c2.joined()); ++i) {
    Pump(server, sworld, c1, w1, c2, w2);
  }
  Check("both clients joined", c1.joined() && c2.joined());

  // Players spawn a couple units apart, so both bubbles overlap both avatars.
  // A prop close by lands in both bubbles; one 500 units out lands in neither.
  rx::u64 near_id = 0;
  rx::u64 far_id = 0;
  SpawnProp(sworld, 5.0f, 1.5f, &near_id);
  ecs::Entity far_prop = SpawnProp(sworld, 500.0f, 0.0f, &far_id);

  for (int i = 0; i < 90; ++i) Pump(server, sworld, c1, w1, c2, w2);

  // 2 player avatars + the near prop; the far prop must not replicate.
  Check("client 1 sees only its bubble (2 players + near prop)",
        c1.replicated_entity_count() == 3);
  Check("client 2 sees only its bubble (2 players + near prop)",
        c2.replicated_entity_count() == 3);
  Check("near prop reached client 1", c1.engine().replicated_entity_count() == 3);

  // Every client mirrors the whole session's bubbles for HUDs/visualizers.
  Check("client 1 mirrors both bubbles", c1.engine().bubbles().size() == 2);
  Check("client 2 mirrors both bubbles", c2.engine().bubbles().size() == 2);

  // Both bubbles cover the near prop; exactly one peer owns it, and the
  // owner is stable while it holds the prop.
  const rx::u32 owner = server.engine().interest().OwnerOf(near_id);
  Check("overlapping bubbles agree on one owner", owner != net::kNoPeer);
  for (int i = 0; i < 30; ++i) Pump(server, sworld, c1, w1, c2, w2);
  Check("ownership is sticky across ticks",
        server.engine().interest().OwnerOf(near_id) == owner);

  // Walk the far prop into range: it must spawn on the clients.
  if (scene::Transform* t = sworld.Get<scene::Transform>(far_prop)) {
    t->position[0] = 6.0f;
    t->position[2] = 1.5f;
  }
  for (int i = 0; i < 90; ++i) Pump(server, sworld, c1, w1, c2, w2);
  Check("prop entering the bubble spawns on client 1",
        c1.replicated_entity_count() == 4);
  Check("prop entering the bubble spawns on client 2",
        c2.replicated_entity_count() == 4);

  // Walk it back out: it must despawn on the clients even though it is alive
  // on the server (that is the bandwidth cut).
  if (scene::Transform* t = sworld.Get<scene::Transform>(far_prop)) {
    t->position[0] = 500.0f;
    t->position[2] = 0.0f;
  }
  for (int i = 0; i < 90; ++i) Pump(server, sworld, c1, w1, c2, w2);
  Check("prop leaving the bubble despawns on client 1",
        c1.replicated_entity_count() == 3);
  Check("prop leaving the bubble despawns on client 2",
        c2.replicated_entity_count() == 3);
  Check("server still owns the prop entity", sworld.IsAlive(far_prop));

  // The whole point: fewer records shipped than full visibility would cost.
  const net::NetStats& stats = server.engine().stats();
  Check("interest filtering cut the replicated records",
        stats.snapshot_records_sent < stats.broadcast_equiv_records);
  std::printf("  stats: %llu records sent, %llu at full visibility (%.0f%%)\n",
              static_cast<unsigned long long>(stats.snapshot_records_sent),
              static_cast<unsigned long long>(stats.broadcast_equiv_records),
              100.0 * static_cast<double>(stats.snapshot_records_sent) /
                  static_cast<double>(stats.broadcast_equiv_records));

  if (g_failures == 0) {
    std::printf("bubble_nettest: all passed\n");
    return 0;
  }
  std::printf("bubble_nettest: %d failure(s)\n", g_failures);
  return 1;
}
