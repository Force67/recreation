using System;

namespace Recreation.Games.Starfield;

// The three resistance ratings a Starfield spacesuit (and the wearer) carries,
// one per damage channel.
public readonly struct SuitResistance(float physical, float energy, float electromagnetic)
{
    public float Physical { get; } = physical;
    public float Energy { get; } = energy;
    public float Electromagnetic { get; } = electromagnetic;

    // The rating that meets a given damage channel.
    public float For(StarfieldDamageType type) => type switch
    {
        StarfieldDamageType.Physical => Physical,
        StarfieldDamageType.Energy => Energy,
        StarfieldDamageType.Electromagnetic => Electromagnetic,
        _ => Physical,
    };
}

// Starfield's damage-mitigation maths: how a spacesuit's Physical / Energy / EM
// resistance blunts a hit on its channel, the Starfield twin of Skyrim's
// DamageMitigation and Fallout's DamageResistance. Each resistance rating resists
// a percent of the hit that climbs with the rating but stops at a cap, so a maxed
// suit shrugs off most of a hit yet never grants full immunity. Pure formulas a
// mod applies when it deals damage; the constants are the vanilla feel, tunable.
public static class DamageMitigation
{
    // Each point of resistance resists this percent of a hit on its channel, up
    // to the cap. The defaults reach the cap at 1600 rating, the upper band of a
    // fully-modded endgame suit.
    public static float ResistScaling { get; set; } = 0.05f;
    public static float ResistCapPercent { get; set; } = 80f;

    // Percent of a hit that `resistance` resists, clamped to the cap.
    public static float ResistPercent(float resistance) =>
        Math.Clamp(resistance * ResistScaling, 0f, ResistCapPercent);

    // The damage that gets through `resistance` on one channel.
    public static float After(float rawDamage, float resistance) =>
        rawDamage <= 0f ? 0f : rawDamage * (1f - ResistPercent(resistance) / 100f);

    // The damage that gets through a `suit` on a hit of `type`.
    public static float After(float rawDamage, SuitResistance suit, StarfieldDamageType type) =>
        After(rawDamage, suit.For(type));
}
