using System;
using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// How two actors regard each other, on Skyrim's relationship-rank scale.
public enum RelationshipRank
{
    Archenemy = -4,
    Enemy = -3,
    Foe = -2,
    Rival = -1,
    Acquaintance = 0,
    Friend = 1,
    Confidant = 2,
    Ally = 3,
    Lover = 4,
}

// The social graph between actors: a registry of relationship ranks the engine
// does not model on its own, the basis for followers, friends, rivals and the
// disposition that gates trade, dialogue and aggression. A relationship is mutual,
// so the order of the pair does not matter. Self-contained runtime state (no
// engine resources), so it needs no teardown hook; tests clear it.
public static class Relationships
{
    private static readonly Dictionary<(ulong, ulong), RelationshipRank> Ranks = new();

    // Sets how `a` and `b` regard each other (mutual).
    public static void Set(Actor a, Actor b, RelationshipRank rank) => Ranks[Key(a, b)] = rank;

    // Their relationship, defaulting to Acquaintance (neutral) when unset.
    public static RelationshipRank Get(Actor a, Actor b) =>
        Ranks.TryGetValue(Key(a, b), out RelationshipRank rank) ? rank : RelationshipRank.Acquaintance;

    public static bool AreFriendly(Actor a, Actor b) => Get(a, b) >= RelationshipRank.Friend;
    public static bool AreHostile(Actor a, Actor b) => Get(a, b) <= RelationshipRank.Foe;

    // A 0..100 disposition derived from the rank, on the familiar Skyrim scale:
    // Acquaintance is a neutral 50 and each rank shifts it twelve points.
    public static int Disposition(Actor a, Actor b) =>
        Math.Clamp(50 + (int)Get(a, b) * 12, 0, 100);

    public static int Count => Ranks.Count;

    public static void Clear() => Ranks.Clear();

    // An order-independent key so a relationship reads the same both ways.
    private static (ulong, ulong) Key(Actor a, Actor b) =>
        a.Handle < b.Handle ? (a.Handle, b.Handle) : (b.Handle, a.Handle);
}
