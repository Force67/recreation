using System;
using Recreation.Games.Fallout;

namespace Recreation.Tests;

// Covers Fallout 4 legendary weapon effects: Instigating doubles a hit on a
// full-health target, Two Shot adds the base again, Furious stacks with
// consecutive hits up to its cap, Bloodied scales with the attacker's missing
// health, and Wounding adds flat bleed on top of an unchanged impact.
public static class FalloutLegendaryEffectsTests
{
    public static void Run(Check check)
    {
        // None passes the hit through unchanged.
        Near(check, "no effect changes nothing", 100f,
             LegendaryEffects.Resolve(100, LegendaryEffect.None));
        check.Equal("no base, no damage", 0f, LegendaryEffects.Resolve(0, LegendaryEffect.TwoShot));

        // Instigating: 2x against a full-health target, plain otherwise.
        Near(check, "Instigating doubles a full-health target", 200f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Instigating, targetHealthFraction: 1f));
        Near(check, "Instigating is plain against a hurt target", 100f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Instigating, targetHealthFraction: 0.9f));

        // Two Shot adds the base again.
        Near(check, "Two Shot adds the base again", 200f,
             LegendaryEffects.Resolve(100, LegendaryEffect.TwoShot));

        // Furious stacks +15% per consecutive hit, capped at 25 stacks.
        Near(check, "Furious adds 60% after four hits", 160f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Furious, consecutiveHits: 4));
        Near(check, "Furious caps at 25 stacks", 475f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Furious, consecutiveHits: 100));

        // Bloodied scales with the attacker's missing health.
        Near(check, "Bloodied peaks at near death", 195f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Bloodied, attackerHealthFraction: 0f));
        Near(check, "Bloodied is half-effect at half health", 147.5f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Bloodied, attackerHealthFraction: 0.5f));
        Near(check, "Bloodied is nothing at full health", 100f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Bloodied, attackerHealthFraction: 1f));

        // Wounding leaves the impact alone and adds bleed on the side.
        Near(check, "Wounding does not change the impact", 100f,
             LegendaryEffects.Resolve(100, LegendaryEffect.Wounding));
        Near(check, "Wounding adds 25 bleed", 25f,
             LegendaryEffects.BleedDamage(LegendaryEffect.Wounding));
        Near(check, "other effects add no bleed", 0f,
             LegendaryEffects.BleedDamage(LegendaryEffect.TwoShot));
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
