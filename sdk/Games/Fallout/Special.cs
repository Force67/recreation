using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// The seven Fallout 4 SPECIAL attributes. Each runs 1..10 and shapes the derived
// stats and perks a build leans on.
public enum SpecialAttribute
{
    Strength,
    Perception,
    Endurance,
    Charisma,
    Intelligence,
    Agility,
    Luck,
}

// Raised when a SPECIAL attribute's rank changes, so derived systems and the HUD
// can react.
public readonly struct SpecialChanged(ulong actorHandle, SpecialAttribute attribute, int rank)
    : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public SpecialAttribute Attribute { get; } = attribute;
    public int Rank { get; } = rank;
}

// Fallout 4's SPECIAL attributes as managed soft logic, the counterpart to Skyrim's
// race traits: a 1..10 rank per attribute per actor, with the derived effects the
// SDK can honour applied straight onto actor values. The engine has no SPECIAL stat,
// so this registry is the authority; the derived bonuses are held as Effects so they
// re-size when a rank changes and revert on Clear.
//
// Faithful derivations Fallout 4 uses:
//   Strength  -> CarryWeight: base 200 + 10 per point (the +10 lbs / STR rule).
//   Endurance -> max Health:  +5 Health per point (the per-END health bump).
//   Intelligence -> XP rate:  +3% XP per point over 1 (the "INT speeds leveling"
//                             rule), folded into CharacterProgress.XpMultiplier.
// Charisma/Perception/Agility/Luck feed perks and checks the SDK does not model, so
// they are tracked (a check or a future system reads them) without a fake derived
// stat invented for them.
public static class Special
{
    // Derived-stat tuning, matching Fallout 4's formulas.
    public static float CarryWeightBase { get; set; } = 200f;
    public static float CarryWeightPerStrength { get; set; } = 10f;
    public static float HealthPerEndurance { get; set; } = 5f;
    public static float XpPercentPerIntelligence { get; set; } = 0.03f;

    private const int MinRank = 1;
    private const int MaxRank = 10;

    private static readonly Dictionary<(ulong, SpecialAttribute), int> Ranks = new();
    private static readonly Dictionary<(ulong, string), Effect> Derived = new();  // actor+av -> bonus

    // The attribute's rank for an actor, defaulting to the Fallout 4 starting 1.
    public static int Rank(Actor actor, SpecialAttribute attribute) =>
        Ranks.TryGetValue((actor.Handle, attribute), out int r) ? r : MinRank;

    // Sets `attribute` to `rank` (clamped to 1..10), recomputing the derived value
    // it drives. Returns the clamped rank.
    public static int Set(Actor actor, SpecialAttribute attribute, int rank)
    {
        rank = Math.Clamp(rank, MinRank, MaxRank);
        if (Rank(actor, attribute) == rank) return rank;
        Ranks[(actor.Handle, attribute)] = rank;
        ApplyDerived(actor, attribute);
        EventBus.Publish(new SpecialChanged(actor.Handle, attribute, rank));
        return rank;
    }

    // Raises `attribute` by one point (the perk-style SPECIAL increase), up to 10.
    // Returns whether it rose.
    public static bool Increase(Actor actor, SpecialAttribute attribute)
    {
        int current = Rank(actor, attribute);
        if (current >= MaxRank) return false;
        Set(actor, attribute, current + 1);
        return true;
    }

    // The CarryWeight a Strength rank yields, the value the carry-weight system caps
    // against. Exposed so a test or HUD can read it without re-deriving the formula.
    public static float CarryWeightFor(int strength) =>
        CarryWeightBase + strength * CarryWeightPerStrength;

    // Resizes the actor-value bonus an attribute drives, held as an Effect so it
    // composes with other modifiers and reverts cleanly. Attributes with no
    // SDK-backed derivation (the social/perk ones) record nothing.
    private static void ApplyDerived(Actor actor, SpecialAttribute attribute)
    {
        switch (attribute)
        {
            case SpecialAttribute.Strength:
                Hold(actor, ActorValue.CarryWeight,
                     Rank(actor, attribute) * CarryWeightPerStrength);
                break;
            case SpecialAttribute.Endurance:
                Hold(actor, ActorValue.Health,
                     Rank(actor, attribute) * HealthPerEndurance);
                break;
            case SpecialAttribute.Intelligence:
                // INT raises the global XP rate rather than an actor value.
                CharacterProgress.XpMultiplier =
                    1f + (Rank(actor, attribute) - MinRank) * XpPercentPerIntelligence;
                break;
        }
    }

    private static void Hold(Actor actor, string actorValue, float magnitude)
    {
        var key = (actor.Handle, actorValue);
        if (Derived.Remove(key, out Effect? old)) Effects.Remove(old);
        if (magnitude != 0f)
            Derived[key] = Effects.Apply(actor, actorValue, magnitude, durationSeconds: 0f);
    }

    public static void Clear()
    {
        foreach (Effect bonus in Derived.Values) Effects.Remove(bonus);
        Derived.Clear();
        Ranks.Clear();
        CharacterProgress.XpMultiplier = 1f;
    }
}
