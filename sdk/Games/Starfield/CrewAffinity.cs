using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// How a companion feels about the player's conduct, on Starfield's affinity ladder.
// The extremes (Admire / Hate) are the campaign endpoints a companion's arc reaches.
public enum AffinityTier
{
    Hate = -2,
    Dislike = -1,
    Neutral = 0,
    Like = 1,
    Admire = 2,
}

// Raised when a companion's affinity crosses into a new tier.
public readonly struct AffinityThresholdChanged(ulong crewHandle, AffinityTier tier) : IGameEvent
{
    public ulong CrewHandle { get; } = crewHandle;
    public AffinityTier Tier { get; } = tier;
}

// Raised once, the first time a companion reaches Admire and their perk unlocks.
public readonly struct CompanionPerkUnlocked(ulong crewHandle) : IGameEvent
{
    public ulong CrewHandle { get; } = crewHandle;
}

// Starfield's signature companion system: a crew member watches what the player
// does and gains or loses affinity for it (Sarah Morgan admires mercy, Sam Coe
// disapproves of theft). This is the social graph the game leans on, the
// counterpart of Skyrim's Relationships but a moving meter rather than a fixed
// rank: a deed calls Approve or Disapprove, the meter crosses the like/admire (or
// dislike/hate) thresholds, and reaching Admire the first time permanently unlocks
// that companion's perk. The engine models no companion-opinion stat, so the affinity
// lives here as managed soft logic. Persistent player state; tests clear it.
public static class CrewAffinity
{
    // The affinity each tier begins at, on the meter the deeds move. Symmetric so a
    // like and a dislike sit the same distance from neutral.
    public static float LikeAt { get; set; } = 30f;
    public static float AdmireAt { get; set; } = 80f;
    public static float DislikeAt { get; set; } = -30f;
    public static float HateAt { get; set; } = -80f;

    // The meter is bounded so a long companionship cannot drift unboundedly.
    public static float Floor { get; set; } = -100f;
    public static float Ceiling { get; set; } = 100f;

    private static readonly Dictionary<ulong, float> Meter = new();
    private static readonly HashSet<ulong> PerkUnlocked = new();

    public static float Of(Actor crew) => Meter.GetValueOrDefault(crew.Handle);

    public static AffinityTier TierOf(Actor crew) => TierFor(Of(crew));

    // The tier an affinity value falls in.
    public static AffinityTier TierFor(float affinity) =>
        affinity >= AdmireAt ? AffinityTier.Admire :
        affinity >= LikeAt ? AffinityTier.Like :
        affinity <= HateAt ? AffinityTier.Hate :
        affinity <= DislikeAt ? AffinityTier.Dislike : AffinityTier.Neutral;

    // True once the companion has reached Admire (a one-way unlock that survives a
    // later dip below the threshold, the way the perk stays earned).
    public static bool HasPerk(Actor crew) => PerkUnlocked.Contains(crew.Handle);

    // The companion approves of the player's last action, raising affinity.
    public static void Approve(Actor crew, float amount = 10f) => Adjust(crew, MathF.Abs(amount));

    // The companion disapproves, lowering affinity.
    public static void Disapprove(Actor crew, float amount = 10f) => Adjust(crew, -MathF.Abs(amount));

    private static void Adjust(Actor crew, float delta)
    {
        if (delta == 0f) return;
        AffinityTier before = TierOf(crew);
        float updated = Math.Clamp(Of(crew) + delta, Floor, Ceiling);
        Meter[crew.Handle] = updated;
        AffinityTier after = TierFor(updated);
        if (after == before) return;

        EventBus.Publish(new AffinityThresholdChanged(crew.Handle, after));
        // Reaching Admire the first time earns the companion perk, permanently.
        if (after == AffinityTier.Admire && PerkUnlocked.Add(crew.Handle))
            EventBus.Publish(new CompanionPerkUnlocked(crew.Handle));
    }

    public static void Clear()
    {
        Meter.Clear();
        PerkUnlocked.Clear();
    }
}
