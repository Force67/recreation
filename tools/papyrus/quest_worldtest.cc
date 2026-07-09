// quest_worldtest: checks the quest world-provenance layer -- spawning entities
// into a real ECS world, moving/disabling them, and rolling back everything a
// quest created or changed via CleanupQuest. Headless (no renderer), so it runs
// in the ctest gate.

#include <cstdint>
#include <cstdio>

#include "core/types.h"
#include "ecs/world.h"
#include "world/components.h"
#include "world/quest_world.h"

// Handles are addressed with std::uint64_t here rather than rx::u64: linking the
// world (-> physics -> arch_types) makes the bare name `u64` ambiguous in this
// global scope. The WorldCommand fields are rx::u64 and convert implicitly.
using Handle = std::uint64_t;

using namespace rx;
using rx::world::Hidden;
using rx::world::QuestWorld;
using rx::world::Transform;
using rx::world::WorldCommand;
using rx::world::WorldCommandQueue;
using rx::world::WorldOp;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

WorldCommand Spawn(Handle quest, Handle handle, f32 x, f32 y, f32 z) {
  WorldCommand c;
  c.op = WorldOp::kSpawn;
  c.quest = quest;
  c.handle = handle;
  c.pos = {x, y, z};
  return c;
}

WorldCommand Move(Handle quest, Handle handle, f32 x, f32 y, f32 z) {
  WorldCommand c;
  c.op = WorldOp::kMove;
  c.quest = quest;
  c.handle = handle;
  c.pos = {x, y, z};
  return c;
}

WorldCommand Cleanup(Handle quest) {
  WorldCommand c;
  c.op = WorldOp::kCleanupQuest;
  c.quest = quest;
  return c;
}

f32 PosX(ecs::World& w, ecs::Entity e) { return w.Get<Transform>(e)->position[0]; }

}  // namespace

int main() {
  std::printf("quest_worldtest\n");
  ecs::World world;
  QuestWorld qw(world);
  WorldCommandQueue q;

  const Handle Q1 = 0x0100ABCD, Q2 = 0x0100BEEF;
  const Handle H1 = 0xFF000001, H2 = 0xFF000002, H3 = 0xFF000003;

  // Two quests spawn three entities between them.
  q.Push(Spawn(Q1, H1, 1, 2, 3));
  q.Push(Spawn(Q1, H2, 4, 5, 6));
  q.Push(Spawn(Q2, H3, 7, 8, 9));
  qw.Apply(q);
  Check("three entities tracked", qw.tracked_entities() == 3);
  Check("H1 alive and registered", world.IsAlive(qw.Find(H1)));
  Check("H1 at its spawn position", PosX(world, qw.Find(H1)) == 1.0f);

  // Mutations under Q1: move H1, disable H2.
  q.Push(Move(Q1, H1, 10, 10, 10));
  WorldCommand disable;
  disable.op = WorldOp::kSetEnabled;
  disable.quest = Q1;
  disable.handle = H2;
  disable.enabled = false;
  q.Push(disable);
  qw.Apply(q);
  Check("H1 moved", PosX(world, qw.Find(H1)) == 10.0f);
  Check("H2 hidden after Disable", world.Has<Hidden>(qw.Find(H2)));

  // Roll back Q1 entirely: its spawns vanish, its mutations are moot.
  q.Push(Cleanup(Q1));
  qw.Apply(q);
  Check("H1 destroyed by cleanup", !world.IsAlive(qw.Find(H1)));
  Check("H2 destroyed by cleanup", !world.IsAlive(qw.Find(H2)));
  Check("only Q2's entity remains tracked", qw.tracked_entities() == 1);
  Check("H3 (other quest) untouched", world.IsAlive(qw.Find(H3)) && PosX(world, qw.Find(H3)) == 7.0f);
  Check("Q1 provenance cleared", qw.quests_with_effects() == 1);  // only Q2 left

  // A quest that MOVES a pre-existing (cell-streamed) reference must restore it,
  // not destroy it, on cleanup.
  const Handle Q3 = 0x0100CAFE, REFR = 0x00012345;
  ecs::Entity pre = world.Create();
  Transform pt;
  pt.position[0] = 100;
  world.Add(pre, pt);
  qw.Register(REFR, pre);
  q.Push(Move(Q3, REFR, 200, 0, 0));
  qw.Apply(q);
  Check("pre-existing ref moved to 200", PosX(world, pre) == 200.0f);
  q.Push(Cleanup(Q3));
  qw.Apply(q);
  Check("pre-existing ref restored, not destroyed",
        world.IsAlive(pre) && PosX(world, pre) == 100.0f);

  // Player move routes through the host hook, carrying the destination ref so
  // the runtime can switch cells when it names an interior.
  bool moved = false;
  f32 player_x = 0;
  rx::u64 dest_ref = 0;
  qw.set_on_move_player([&](rx::u64 ref, f32 x, f32, f32) {
    moved = true;
    player_x = x;
    dest_ref = ref;
  });
  WorldCommand pm;
  pm.op = WorldOp::kMovePlayer;
  pm.handle = 0xABCD;  // destination reference handle
  pm.pos = {42, 0, 0};
  q.Push(pm);
  qw.Apply(q);
  Check("player-move hook fired with target + dest ref",
        moved && player_x == 42.0f && dest_ref == 0xABCD);

  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
