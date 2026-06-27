using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers spawning a leveled list: resolving an actor (or item) list and placing
// each result at an origin, the basis for encounter and loot-drop mods.
public static class LeveledSpawnTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong banditList = 0x100, banditBase = 0x200, origin = 0x14;
        // A single level-1 entry, so the pick is deterministic.
        fake.SetLeveledList(banditList, chanceNone: 0, flags: 0, (banditBase, 1, 1));

        var spawnPoint = ObjectReference.From(origin);
        var spawned = LeveledList.From(banditList).SpawnAt(spawnPoint, level: 5);

        check.Equal("one actor spawned", 1, spawned.Count);
        check.Equal("placed the resolved base", banditBase, fake.Spawned[0].Base);
        check.Equal("placed at the origin", origin, fake.Spawned[0].Origin);
        check.Equal("placed the resolved count", 1, fake.Spawned[0].Count);
        check.That("the spawned reference is real", spawned[0].Exists);

        // A loot list spawns the rolled item with its stack count.
        const ulong lootList = 0x101, gold = 0x201;
        fake.SetLeveledList(lootList, chanceNone: 0, flags: 0, (gold, 1, 25));  // 25 gold
        LeveledList.From(lootList).SpawnAt(spawnPoint, level: 5);
        check.Equal("the loot stack count carries", 25, fake.Spawned[1].Count);
        check.Equal("the loot base is the gold", gold, fake.Spawned[1].Base);

        Native.Backend = null;
    }
}
