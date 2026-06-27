using System;
using System.Linq;
using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the leveled-list resolver: the chance of nothing, level gating, the
// all-levels and each-in-count flags, and recursion through sublists.
public static class LeveledListTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        var rng = new Random(1);

        const ulong itemA = 0xA1, itemB = 0xB1, itemJ = 0xC1;

        // A 100% chance of nothing yields nothing.
        const ulong none = 0x100;
        fake.SetLeveledList(none, 100, 0, (itemA, 1, 1));
        check.Equal("chance-none yields nothing", 0, LeveledList.From(none).Resolve(10, rng).Count);

        // A single eligible entry resolves to that item, carrying its count.
        const ulong single = 0x101;
        fake.SetLeveledList(single, 0, 0, (itemA, 1, 2));
        var one = LeveledList.From(single).Resolve(10, rng);
        check.Equal("single entry resolves to one stack", 1, one.Count);
        check.Equal("resolved item is A", itemA, one[0].Item.Handle);
        check.Equal("resolved count carries", 2, one[0].Count);

        // Without the all-levels flag the highest eligible tier wins.
        const ulong tiered = 0x102;
        fake.SetLeveledList(tiered, 0, 0, (itemA, 1, 1), (itemB, 5, 1));
        check.Equal("below tier B picks A", itemA,
                    LeveledList.From(tiered).Resolve(3, rng).Single().Item.Handle);
        check.Equal("at tier B picks B", itemB,
                    LeveledList.From(tiered).Resolve(10, rng).Single().Item.Handle);

        // A list with nothing eligible yields nothing.
        const ulong highOnly = 0x103;
        fake.SetLeveledList(highOnly, 0, 0, (itemB, 50, 1));
        check.Equal("nothing eligible yields nothing", 0,
                    LeveledList.From(highOnly).Resolve(10, rng).Count);

        // Recursion: an entry that is a sublist resolves through it.
        const ulong sub = 0x200, parent = 0x201;
        fake.SetLeveledList(sub, 0, 0, (itemJ, 1, 3));
        fake.SetLeveledList(parent, 0, 0, (sub, 1, 1));
        var recursed = LeveledList.From(parent).Resolve(10, rng);
        check.Equal("recursion resolves through the sublist", 1, recursed.Count);
        check.Equal("recursed item is J", itemJ, recursed[0].Item.Handle);
        check.Equal("recursed count carries", 3, recursed[0].Count);

        // The each-in-count flag rolls a sublist entry once per count.
        const ulong leaf = 0x202, eachParent = 0x203;
        fake.SetLeveledList(leaf, 0, 0, (itemJ, 1, 1));
        fake.SetLeveledList(eachParent, 0, 0x02, (leaf, 1, 3));  // 0x02 = each in count
        check.Equal("each-in-count rolls per count", 3,
                    LeveledList.From(eachParent).Resolve(10, rng).Count);

        // The all-levels flag keeps low-level entries eligible at high level.
        const ulong allLevels = 0x204;
        fake.SetLeveledList(allLevels, 0, 0x01, (itemA, 1, 1), (itemB, 5, 1));  // 0x01 = all levels
        bool sawA = false;
        for (int i = 0; i < 100 && !sawA; i++)
            sawA = LeveledList.From(allLevels).Resolve(10, rng).Single().Item.Handle == itemA;
        check.That("all-levels keeps low entries eligible", sawA);

        Native.Backend = null;
    }
}
