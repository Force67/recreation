using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// The four main Commonwealth factions the player can ally with or turn against.
public enum CommonwealthFaction
{
    Minutemen,
    BrotherhoodOfSteel,
    Railroad,
    Institute,
}

// How a faction regards the player, derived from the numeric reputation. Fallout 4
// gates faction perks and dialogue on standing; these are the bands that drives.
public enum FactionStanding
{
    Hostile,
    Neutral,
    Friendly,
    Allied,
}

// Raised when the player's standing with a faction crosses into a new band.
public readonly struct FactionStandingChanged(CommonwealthFaction faction, FactionStanding standing,
                                              int reputation) : IGameEvent
{
    public CommonwealthFaction Faction { get; } = faction;
    public FactionStanding Standing { get; } = standing;
    public int Reputation { get; } = reputation;
}

// Faction reputation as managed soft logic: a numeric standing (-100..100) per
// Commonwealth faction that play moves up and down, banded into a coarse standing
// the questing and dialogue layers gate on. Helping the Minutemen retake a
// settlement raises them; siding with the Institute against the Railroad sinks the
// Railroad. The four factions are largely mutually exclusive, so a quest that pays
// one can dock its rival in the same call (GainWithRival). The engine has no faction
// reputation, so this registry is the authority; quests and the public API drive it.
// Mirrors Skyrim's Relationships numeric-value pattern. Persistent player state, so
// it outlives the mod-host lifecycle; tests clear it.
public static class FactionReputation
{
    public const int Min = -100;
    public const int Max = 100;

    // Band thresholds on the -100..100 scale.
    public static int HostileBelow { get; set; } = -25;
    public static int FriendlyAt { get; set; } = 25;
    public static int AlliedAt { get; set; } = 75;

    private static readonly Dictionary<CommonwealthFaction, int> Reputation = new();

    public static int Of(CommonwealthFaction faction) => Reputation.GetValueOrDefault(faction);

    public static FactionStanding StandingOf(CommonwealthFaction faction)
    {
        int rep = Of(faction);
        if (rep >= AlliedAt) return FactionStanding.Allied;
        if (rep >= FriendlyAt) return FactionStanding.Friendly;
        if (rep < HostileBelow) return FactionStanding.Hostile;
        return FactionStanding.Neutral;
    }

    // Adds `amount` reputation (negative to dock it), clamped, announcing a band
    // crossing. Returns the new reputation.
    public static int Gain(CommonwealthFaction faction, int amount)
    {
        FactionStanding before = StandingOf(faction);
        int rep = Math.Clamp(Of(faction) + amount, Min, Max);
        Reputation[faction] = rep;
        FactionStanding after = StandingOf(faction);
        if (after != before)
            EventBus.Publish(new FactionStandingChanged(faction, after, rep));
        return rep;
    }

    // Pays `faction` while docking its `rival` by the same amount, the cost of
    // taking sides in the Commonwealth's mutually-exclusive allegiances.
    public static void GainWithRival(CommonwealthFaction faction, CommonwealthFaction rival, int amount)
    {
        Gain(faction, amount);
        Gain(rival, -amount);
    }

    public static bool IsAllied(CommonwealthFaction faction) =>
        StandingOf(faction) == FactionStanding.Allied;

    public static bool IsHostile(CommonwealthFaction faction) =>
        StandingOf(faction) == FactionStanding.Hostile;

    public static void Clear() => Reputation.Clear();
}
