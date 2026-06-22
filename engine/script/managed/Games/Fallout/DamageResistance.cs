using System;

namespace Recreation.Games.Fallout;

// The channel a Fallout 4 hit lands on, each met by its own resistance actor
// value (DamageResist / EnergyResist / RadResist).
public enum DamageType
{
    Physical,
    Energy,
    Radiation,
}

// Fallout 4's damage-mitigation maths: how a Damage / Energy / Radiation
// Resistance blunts an incoming hit on its channel. Where Skyrim's armor resists
// a flat percent per point, Fallout's curve is a ratio of the hit to the
// resistance, so the same resistance shrugs off a chip hit almost entirely yet
// barely dents a heavy one (the GECK "damage threshold" feel without a hard
// threshold). A floor guarantees every hit still deals a sliver of chip damage,
// so an over-armored target is never fully immune. Pure formulas a mod applies
// when it deals damage; the constants are the vanilla values, tunable for a
// different balance.
public static class DamageResistance
{
    // The published Fallout 4 curve. With resistance R against incoming damage D,
    // the fraction that gets through is (D / (D + R*Scale))^Exponent, so:
    //   - R == 0 lets the full hit through,
    //   - R == D lets just under half through (with the vanilla 1.0 scale),
    //   - R >> D shrinks the hit toward the chip floor.
    // The exponent steepens the curve the way Bethesda's "soft threshold" does:
    // armor matters more the closer it sits to the incoming damage.
    public static float ResistScale { get; set; } = 1f;
    public static float Exponent { get; set; } = 0.367f;

    // Every hit lands at least this fraction of its raw damage, so stacking
    // resistance never grants full immunity (the in-game ~minimum-damage rule).
    public static float MinDamageFraction { get; set; } = 0.01f;

    // The fraction of `rawDamage` that gets through `resistance` on one channel,
    // in [MinDamageFraction, 1]. A non-positive resistance changes nothing.
    public static float ThroughFraction(float rawDamage, float resistance)
    {
        if (rawDamage <= 0f) return 0f;
        if (resistance <= 0f) return 1f;
        float through = MathF.Pow(rawDamage / (rawDamage + resistance * ResistScale), Exponent);
        return Math.Clamp(through, MinDamageFraction, 1f);
    }

    // The damage that gets through `resistance` on one channel.
    public static float After(float rawDamage, float resistance) =>
        rawDamage <= 0f ? 0f : rawDamage * ThroughFraction(rawDamage, resistance);

    // The damage `who` actually takes from a `rawDamage` hit of `type`, reading
    // the matching resistance off their actor values.
    public static float AfterFor(Actor who, float rawDamage, DamageType type) =>
        After(rawDamage, who.GetValue(ResistValueFor(type)));

    // The Fallout 4 actor-value id of the resistance that meets a damage type.
    public static string ResistValueFor(DamageType type) => type switch
    {
        DamageType.Physical => FalloutActorValue.DamageResist,
        DamageType.Energy => FalloutActorValue.EnergyResist,
        DamageType.Radiation => FalloutActorValue.RadResist,
        _ => FalloutActorValue.DamageResist,
    };
}
