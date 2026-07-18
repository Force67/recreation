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

WorldCommand SetDoor(WorldOp op, Handle quest, Handle handle, bool value) {
  WorldCommand c;
  c.op = op;
  c.quest = quest;
  c.handle = handle;
  c.enabled = value;
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
  Check("H3 (other quest) untouched",
        world.IsAlive(qw.Find(H3)) && PosX(world, qw.Find(H3)) == 7.0f);
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

  world.Add(pre, world::DoorState{});
  WorldCommand lock;
  lock.op = WorldOp::kSetLocked;
  lock.quest = Q3;
  lock.handle = REFR;
  lock.enabled = true;
  WorldCommand open = lock;
  open.op = WorldOp::kSetOpen;
  q.Push(lock);
  q.Push(open);
  qw.Apply(q);
  Check("door mutations reach ECS",
        world.Get<world::DoorState>(pre)->locked && world.Get<world::DoorState>(pre)->open);
  const std::vector<WorldCommand> door_snapshot = qw.SnapshotDoorStates();
  Check("door mutations are available for join snapshots", door_snapshot.size() == 4);

  ecs::World join_world;
  QuestWorld join_qw(join_world);
  ecs::Entity join_door = join_world.Create();
  join_world.Add(join_door, Transform{});
  join_world.Add(join_door, world::DoorState{});
  join_qw.Register(REFR, join_door);
  join_qw.Apply(door_snapshot);
  Check("late join door replay applies current state",
        join_world.Get<world::DoorState>(join_door)->locked &&
            join_world.Get<world::DoorState>(join_door)->open);
  q.Push(Cleanup(Q3));
  qw.Apply(q);
  join_qw.Apply({Cleanup(Q3)});
  Check("door mutations roll back",
        !world.Get<world::DoorState>(pre)->locked && !world.Get<world::DoorState>(pre)->open);
  Check("late join door replay retains cleanup provenance",
        !join_world.Get<world::DoorState>(join_door)->locked &&
            !join_world.Get<world::DoorState>(join_door)->open);

  // Door effects form a per-property history across quests. Removing an older
  // quest must preserve the newest surviving value, and either cleanup order
  // must eventually return to the authored state.
  const Handle DOOR_Q1 = 0x0100D001, DOOR_Q2 = 0x0100D002;
  qw.Apply({SetDoor(WorldOp::kSetLocked, DOOR_Q1, REFR, true),
            SetDoor(WorldOp::kSetLocked, DOOR_Q2, REFR, false), Cleanup(DOOR_Q1)});
  Check("older lock cleanup preserves the newer quest value",
        !world.Get<world::DoorState>(pre)->locked);

  ecs::World rebased_join_world;
  QuestWorld rebased_join_qw(rebased_join_world);
  ecs::Entity rebased_join_door = rebased_join_world.Create();
  rebased_join_world.Add(rebased_join_door, Transform{});
  rebased_join_world.Add(rebased_join_door, world::DoorState{});
  rebased_join_qw.Register(REFR, rebased_join_door);
  rebased_join_qw.Apply(qw.SnapshotDoorStates());
  qw.Apply({Cleanup(DOOR_Q2)});
  rebased_join_qw.Apply({Cleanup(DOOR_Q2)});
  Check("older-first lock cleanup returns to authored state",
        !world.Get<world::DoorState>(pre)->locked);
  Check("rebased lock provenance survives late join",
        !rebased_join_world.Get<world::DoorState>(rebased_join_door)->locked);

  qw.Apply({SetDoor(WorldOp::kSetLocked, DOOR_Q1, REFR, true),
            SetDoor(WorldOp::kSetLocked, DOOR_Q2, REFR, false), Cleanup(DOOR_Q2)});
  Check("newer lock cleanup reveals the older quest value",
        world.Get<world::DoorState>(pre)->locked);
  qw.Apply({Cleanup(DOOR_Q1)});
  Check("newer-first lock cleanup returns to authored state",
        !world.Get<world::DoorState>(pre)->locked);

  qw.Apply({SetDoor(WorldOp::kSetOpen, DOOR_Q1, REFR, true),
            SetDoor(WorldOp::kSetOpen, DOOR_Q2, REFR, false), Cleanup(DOOR_Q1)});
  Check("older open cleanup preserves the newer quest value",
        !world.Get<world::DoorState>(pre)->open);
  qw.Apply({Cleanup(DOOR_Q2)});
  Check("older-first open cleanup returns to authored state",
        !world.Get<world::DoorState>(pre)->open);

  qw.Apply({SetDoor(WorldOp::kSetOpen, DOOR_Q1, REFR, true),
            SetDoor(WorldOp::kSetOpen, DOOR_Q2, REFR, false), Cleanup(DOOR_Q2)});
  Check("newer open cleanup reveals the older quest value",
        world.Get<world::DoorState>(pre)->open);
  qw.Apply({Cleanup(DOOR_Q1)});
  Check("newer-first open cleanup returns to authored state",
        !world.Get<world::DoorState>(pre)->open);

  // A remote request is authorized from server-owned ECS state, never from the
  // client-supplied handle alone.
  const Handle ACTIVATABLE = 0x00012346;
  ecs::Entity remote_player = world.Create();
  Transform remote_player_transform;
  remote_player_transform.position[0] = 10.0f;
  world.Add(remote_player, remote_player_transform);
  ecs::Entity activatable = world.Create();
  Transform activatable_transform;
  activatable_transform.position[0] = 12.0f;
  world.Add(activatable, activatable_transform);
  qw.Register(ACTIVATABLE, activatable);
  Check("near live target authorizes remote activation",
        qw.CanActivateFrom(remote_player, ACTIVATABLE, 3.5f));
  world.Get<Transform>(activatable)->position[0] = 20.0f;
  Check("distant target rejects remote activation",
        !qw.CanActivateFrom(remote_player, ACTIVATABLE, 3.5f));
  world.Get<Transform>(activatable)->position[0] = 12.0f;
  world.Add(activatable, Hidden{});
  Check("disabled target rejects remote activation",
        !qw.CanActivateFrom(remote_player, ACTIVATABLE, 3.5f));
  world.Remove<Hidden>(activatable);
  world.Add(activatable, world::Deleted{});
  Check("deleted target rejects remote activation",
        !qw.CanActivateFrom(remote_player, ACTIVATABLE, 3.5f));
  world.Remove<world::Deleted>(activatable);
  qw.Unregister(ACTIVATABLE);
  Check("unloaded target rejects remote activation",
        !qw.CanActivateFrom(remote_player, ACTIVATABLE, 3.5f));

  const Handle Q4 = 0x0100D00D, UNLOADED = 0x00054321;
  q.Push(Move(Q4, UNLOADED, 42, 0, 0));
  qw.Apply(q);
  ecs::Entity loaded_late = world.Create();
  world.Add(loaded_late, Transform{});
  qw.Register(UNLOADED, loaded_late);
  Check("unloaded mutation applies on registration", PosX(world, loaded_late) == 42.0f);
  qw.Unregister(UNLOADED);
  world.Destroy(loaded_late);
  q.Push(Cleanup(Q4));
  qw.Apply(q);
  ecs::Entity reloaded = world.Create();
  Transform persisted;
  persisted.position[0] = 42;
  world.Add(reloaded, persisted);
  qw.Register(UNLOADED, reloaded);
  Check("unloaded cleanup restores prior state", PosX(world, reloaded) == 0.0f);

  const size_t pending_before = qw.pending_command_count();
  const Handle COALESCED = 0x00054322;
  for (int i = 0; i < 100; ++i) q.Push(Move(Q4, COALESCED, static_cast<f32>(i), 0, 0));
  qw.Apply(q);
  Check("adjacent deferred state updates coalesce",
        qw.pending_command_count() == pending_before + 1);
  ecs::Entity coalesced = world.Create();
  world.Add(coalesced, Transform{});
  qw.Register(COALESCED, coalesced);
  Check("coalesced deferred state keeps the latest value", PosX(world, coalesced) == 99.0f);

  const Handle BOUNDED = 0x00054323;
  for (size_t i = 0; i < QuestWorld::kMaxPendingCommandsPerHandle + 10; ++i)
    q.Push(Move(0x02000000 + i, BOUNDED, static_cast<f32>(i), 0, 0));
  qw.Apply(q);
  Check("deferred command storage is bounded per handle",
        qw.pending_command_count() == pending_before + QuestWorld::kMaxPendingCommandsPerHandle);
  ecs::Entity bounded = world.Create();
  world.Add(bounded, Transform{});
  qw.Register(BOUNDED, bounded);
  Check("registering a bounded handle drains its deferred commands",
        qw.pending_command_count() == pending_before);

  const Handle INITIALLY_DISABLED = 0x00065432;
  WorldCommand enable;
  enable.op = WorldOp::kSetEnabled;
  enable.handle = INITIALLY_DISABLED;
  enable.enabled = true;
  q.Push(enable);
  qw.Apply(q);
  ecs::Entity disabled_ref = world.Create();
  world.Add(disabled_ref, Transform{});
  world.Add(disabled_ref, Hidden{});
  qw.Register(INITIALLY_DISABLED, disabled_ref);
  Check("deferred Enable unhides an initially-disabled logical ref",
        !world.Has<Hidden>(disabled_ref));
  world.Add(disabled_ref, Hidden{});
  std::vector<std::pair<rx::u64, std::array<f32, 3>>> hidden_positions;
  qw.SnapshotPositions(hidden_positions);
  bool hidden_snapshotted = false;
  for (const auto& [handle, _] : hidden_positions)
    hidden_snapshotted |= handle == INITIALLY_DISABLED;
  Check("disabled refs are absent from simulation snapshots", !hidden_snapshotted);
  world.Remove<Hidden>(disabled_ref);

  const Handle PENDING_DOOR = 0x00076543;
  WorldCommand pending_open;
  pending_open.op = WorldOp::kSetOpen;
  pending_open.handle = PENDING_DOOR;
  pending_open.enabled = true;
  q.Push(pending_open);
  qw.Apply(q);
  bool pending_door_snapshotted = false;
  for (const WorldCommand& command : qw.SnapshotDoorStates())
    pending_door_snapshotted |=
        command.handle == PENDING_DOOR && command.op == WorldOp::kSetOpen && command.enabled;
  Check("pending door state is included in join snapshots", pending_door_snapshotted);

  // Parent-derived visibility must not overwrite a pack-in child's own disabled
  // state. In particular, disabling it while the root is already hidden must be
  // remembered even though the effective Hidden component does not change.
  const Handle Q5 = 0x0100D15A, PACK_ROOT = 0x00076544, PACK_CHILD = 0x00076545;
  ecs::Entity pack_root = world.Create();
  world.Add(pack_root, Transform{});
  world.Add(pack_root, Hidden{});
  qw.Register(PACK_ROOT, pack_root);
  ecs::Entity pack_child = world.Create();
  world.Add(pack_child, Transform{});
  world.Add(pack_child, Hidden{});
  world.Add(pack_child, world::PackInOwner{PACK_ROOT});
  qw.Register(PACK_CHILD, pack_child);
  WorldCommand disable_child;
  disable_child.op = WorldOp::kSetEnabled;
  disable_child.quest = Q5;
  disable_child.handle = PACK_CHILD;
  disable_child.enabled = false;
  q.Push(disable_child);
  qw.Apply(q);
  Check("hidden pack-in child remembers an independent disable",
        world.Get<world::PackInOwner>(pack_child)->independently_hidden);
  q.Push(Cleanup(Q5));
  qw.Apply(q);
  Check("pack-in cleanup restores independent visibility without exposing its hidden parent",
        !world.Get<world::PackInOwner>(pack_child)->independently_hidden &&
            world.Has<Hidden>(pack_child));

  // Streamed deletion keeps the entity alive for the streamer, but removes it
  // from logical lifecycle/proximity immediately and emits unload only once.
  int unloaded = 0;
  qw.set_on_unregister([&](rx::u64) { ++unloaded; });
  const Handle DELETED = 0x00077777;
  ecs::Entity deleted = world.Create();
  world.Add(deleted, Transform{});
  world.Add(deleted, world::FormLink{});
  world.Add(deleted, world::CellMembership{});
  world.Add(deleted, world::Prop{});
  qw.Register(DELETED, deleted);
  WorldCommand erase;
  erase.op = WorldOp::kDelete;
  erase.handle = DELETED;
  q.Push(erase);
  qw.Apply(q);
  std::vector<std::pair<rx::u64, std::array<f32, 3>>> positions;
  qw.SnapshotPositions(positions);
  bool deleted_visible = false;
  for (const auto& [handle, _] : positions) deleted_visible |= handle == DELETED;
  Check("deleted streamed ref stays alive for unload", world.IsAlive(deleted));
  Check("deleted streamed ref is absent from snapshots", !deleted_visible);
  Check("logical delete emits unload", unloaded == 1);
  qw.Unregister(DELETED);
  Check("physical unload does not emit a second unload", unloaded == 1);

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
