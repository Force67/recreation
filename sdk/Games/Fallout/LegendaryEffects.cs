using System;

namespace Recreation.Games.Fallout;

// A Fallout 4 legendary weapon effect: the prefix on a legendary drop that
// rewrites how much damage a hit deals.
public enum LegendaryEffect
{
    None,
    Instigating,  // double damage against a target at full health
    TwoShot,      // fires a second projectile, adding ~the base again
    Furious,      // each consecutive hit on one target adds damage
    Bloodied,     // more damage the lower the attacker's own health
    Wounding,     // adds flat bleed damage over time on top of the hit
}

// Fallout 4 legendary weapon effects: the damage rewrite each prefix applies to a
// hit, resolved from the effect and the fight's context (the target's health, how
// many hits have landed in a row, the attacker's own health). Pure math given the
// inputs, so a mod resolves the real damage of a legendary swing and previews how
// an effect changes it; the magnitudes are the vanilla values, tunable. State
// like the Furious hit count or the target's current health is passed in, not
// held here, keeping the resolver deterministic.
public static class LegendaryEffects
{
    // Instigating: double damage when the target is at full health.
    public static float InstigatingMultiplier { get; set; } = 2f;

    // Two Shot: the second projectile adds this fraction of base (vanilla doubles
    // the raw damage, hit-rolls aside).
    public static float TwoShotBonus { get; set; } = 1f;

    // Furious: each consecutive hit after the first adds this fraction of base,
    // capped so a long burst does not run away.
    public static float FuriousPerHit { get; set; } = 0.15f;
    public static int FuriousMaxStacks { get; set; } = 25;

    // Bloodied: at zero attacker health the hit does this much extra, scaling
    // linearly to nothing at full health (vanilla +95% at near death).
    public static float BloodiedMaxBonus { get; set; } = 0.95f;

    // Wounding: flat bleed damage added on top of the hit (25 over 5s in vanilla).
    public static float WoundingBleed { get; set; } = 25f;

    // The damage of a `baseDamage` hit carrying `effect`, given the fight context.
    //   targetHealthFraction  - the target's current health in [0, 1] (Instigating)
    //   consecutiveHits       - hits landed in a row on this target (Furious)
    //   attackerHealthFraction- the attacker's current health in [0, 1] (Bloodied)
    public static float Resolve(float baseDamage, LegendaryEffect effect,
                                float targetHealthFraction = 1f,
                                int consecutiveHits = 0,
                                float attackerHealthFraction = 1f)
    {
        if (baseDamage <= 0f) return 0f;
        return effect switch
        {
            LegendaryEffect.Instigating =>
                baseDamage * (targetHealthFraction >= 1f ? InstigatingMultiplier : 1f),
            LegendaryEffect.TwoShot => baseDamage * (1f + TwoShotBonus),
            LegendaryEffect.Furious =>
                baseDamage * (1f + FuriousPerHit * Math.Clamp(consecutiveHits, 0, FuriousMaxStacks)),
            LegendaryEffect.Bloodied =>
                baseDamage * (1f + BloodiedMaxBonus * (1f - Math.Clamp(attackerHealthFraction, 0f, 1f))),
            LegendaryEffect.Wounding => baseDamage,  // the bleed is separate, see BleedDamage
            _ => baseDamage,
        };
    }

    // The flat bleed damage a Wounding hit adds on top of its impact, zero for
    // every other effect.
    public static float BleedDamage(LegendaryEffect effect) =>
        effect == LegendaryEffect.Wounding ? WoundingBleed : 0f;
}
