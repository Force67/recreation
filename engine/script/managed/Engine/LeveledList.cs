using System;
using System.Collections.Generic;
using System.Linq;

namespace Recreation;

// One authored entry in a leveled list: a form that becomes eligible once the
// character reaches Level, in the given Count. The form may itself be a leveled
// list, which the resolver recurses into.
public readonly struct LeveledEntry(Form item, int level, int count)
{
    public Form Item { get; } = item;
    public int Level { get; } = level;
    public int Count { get; } = count;
}

// A concrete item a leveled list resolved to: a form and how many of it dropped.
public readonly struct LeveledItem(Form item, int count)
{
    public Form Item { get; } = item;
    public int Count { get; } = count;
}

// A leveled item list (LVLI): the loot, drop and stock table Skyrim resolves by
// character level. Entries at or below the level are eligible; one is picked at
// random and sublists recurse. Reading the record is the engine's job; the
// resolution rules live here so mods can roll vanilla tables or build their own.
//
// Resolution: roll the chance of nothing first; then from the eligible entries
// (level <= the character's) pick one at random -- every eligible entry if the
// AllLevels flag is set, otherwise only those at the highest eligible level. A
// concrete pick yields its Count; a sublist pick recurses, once, or once per
// Count when the EachInCount flag is set.
public sealed class LeveledList : Form
{
    private const int FlagAllLevels = 0x01;    // every entry <= level is eligible
    private const int FlagEachInCount = 0x02;  // resolve a sublist once per count
    private const int MaxDepth = 64;           // stop a malformed circular list

    private LeveledList(ulong handle) : base(handle) => Load();

    public static new LeveledList From(ulong handle) => new(handle);
    public static LeveledList From(Form form) => new(form.Handle);

    // Percent chance the list yields nothing (LVLD).
    public int ChanceNone { get; private set; }
    public IReadOnlyList<LeveledEntry> Entries { get; private set; } = Array.Empty<LeveledEntry>();
    private int _flags;

    public bool AllLevels => (_flags & FlagAllLevels) != 0;
    public bool EachInCount => (_flags & FlagEachInCount) != 0;

    // True if this form is actually a (non-empty) leveled list.
    public bool IsLeveledList => Entries.Count > 0;

    // Reads the entries and flags from the record. Eager, so a wrapper is ready to
    // resolve and a recursion materialises each sublist fully before the engine's
    // shared parse cache moves on.
    private void Load()
    {
        int count = Call("GetLeveledListCount").AsInt();
        ChanceNone = Call("GetLeveledChanceNone").AsInt();
        _flags = Call("GetLeveledFlags").AsInt();
        var entries = new LeveledEntry[count];
        for (int i = 0; i < count; i++)
            entries[i] = new LeveledEntry(Form.From(Call("GetNthLeveledForm", i).AsHandle()),
                                          Call("GetNthLeveledLevel", i).AsInt(),
                                          Call("GetNthLeveledCount", i).AsInt());
        Entries = entries;
    }

    // Resolves the list to concrete items for a character at `level`, using `rng`
    // for the chance-of-nothing roll and the random picks.
    public IReadOnlyList<LeveledItem> Resolve(int level, Random rng)
    {
        var into = new List<LeveledItem>();
        Resolve(level, rng, into, 0);
        return into;
    }

    // Resolves with the shared thread-safe RNG, for callers that do not need a
    // reproducible roll.
    public IReadOnlyList<LeveledItem> Resolve(int level) => Resolve(level, Random.Shared);

    private void Resolve(int level, Random rng, List<LeveledItem> into, int depth)
    {
        if (depth > MaxDepth) return;
        if (ChanceNone > 0 && rng.Next(100) < ChanceNone) return;

        var eligible = Entries.Where(e => e.Level <= level).ToList();
        if (eligible.Count == 0) return;

        List<LeveledEntry> pool = eligible;
        if (!AllLevels)
        {
            int top = eligible.Max(e => e.Level);
            pool = eligible.Where(e => e.Level == top).ToList();
        }

        LeveledEntry pick = pool[rng.Next(pool.Count)];
        LeveledList sub = From(pick.Item);
        if (sub.IsLeveledList)
        {
            int rolls = EachInCount ? Math.Max(1, pick.Count) : 1;
            for (int r = 0; r < rolls; r++) sub.Resolve(level, rng, into, depth + 1);
        }
        else
        {
            into.Add(new LeveledItem(pick.Item, Math.Max(1, pick.Count)));
        }
    }
}
