using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// The player's standing with one of the Settled Systems' factions, the named tiers
// reputation crosses. Hostile is the bottom; Hero is the top a campaign earns.
public enum FactionRank
{
    Hostile = -1,
    Neutral = 0,
    Liked = 1,
    Allied = 2,
    Hero = 3,
}

// Raised when the player's standing with a faction crosses into a new rank.
public readonly struct FactionRankChanged(uint faction, FactionRank rank) : IGameEvent
{
    public uint Faction { get; } = faction;
    public FactionRank Rank { get; } = rank;
}

// Starfield faction reputation: the player builds (or burns) standing with the UC
// Vanguard, the Freestar Collective, Constellation, the Crimson Fleet and Ryujin
// through their questlines. The engine has no live reputation stat for a scripted
// character, so the standing is held here as managed soft logic, the counterpart of
// Skyrim's FactionRelations but a numeric meter rather than authored reactions. A
// quest or deed calls Gain/Lose; crossing a threshold raises the rank and announces
// it. Reputation is clamped to a band so a single act cannot vault straight to Hero.
// Persistent player state, so it outlives the mod-host lifecycle; tests clear it.
public static class FactionReputation
{
    // Reputation needed to hold each rank (the floor of the band). Below Liked's
    // floor and at or above zero is Neutral; below zero is Hostile.
    public static float LikedAt { get; set; } = 25f;
    public static float AlliedAt { get; set; } = 60f;
    public static float HeroAt { get; set; } = 100f;

    // The bounds reputation is held within, so it cannot run away unboundedly.
    public static float Floor { get; set; } = -50f;
    public static float Ceiling { get; set; } = 150f;

    private static readonly Dictionary<uint, float> Standing = new();

    public static float Of(uint faction) => Standing.GetValueOrDefault(faction);

    public static FactionRank RankOf(uint faction) => RankFor(Of(faction));

    // The rank a reputation value falls in.
    public static FactionRank RankFor(float reputation) =>
        reputation >= HeroAt ? FactionRank.Hero :
        reputation >= AlliedAt ? FactionRank.Allied :
        reputation >= LikedAt ? FactionRank.Liked :
        reputation < 0f ? FactionRank.Hostile : FactionRank.Neutral;

    // Earns reputation with a faction (a completed mission, a saved colony), ranking
    // up and announcing it when a threshold is crossed.
    public static void Gain(uint faction, float amount) => Adjust(faction, MathF.Abs(amount));

    // Loses reputation with a faction (a betrayal, a crime against it).
    public static void Lose(uint faction, float amount) => Adjust(faction, -MathF.Abs(amount));

    private static void Adjust(uint faction, float delta)
    {
        if (delta == 0f) return;
        FactionRank before = RankOf(faction);
        float updated = Math.Clamp(Of(faction) + delta, Floor, Ceiling);
        Standing[faction] = updated;
        FactionRank after = RankFor(updated);
        if (after != before) EventBus.Publish(new FactionRankChanged(faction, after));
    }

    public static void Clear() => Standing.Clear();
}
