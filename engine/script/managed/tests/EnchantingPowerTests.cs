using System;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the enchanting-strength calculator: skill and soul size scale an effect's
// magnitude, a bigger soul enchants stronger, and no soul yields nothing.
public static class EnchantingPowerTests
{
    public static void Run(Check check)
    {
        var power = new EnchantingPower();  // defaults: +0.5%/skill, soul charge size/5

        // Full skill and a grand soul give 1.5x; a petty soul a fifth of that.
        check.That("a grand soul at 100 skill", Near(power.Strength(100f, 100f, SoulSize.Grand), 150f));
        check.That("a petty soul at 100 skill", Near(power.Strength(100f, 100f, SoulSize.Petty), 30f));
        check.That("no skill keeps the base with a grand soul",
                   Near(power.Strength(100f, 0f, SoulSize.Grand), 100f));

        // No soul, no enchantment.
        check.Equal("no soul yields nothing", 0f, power.Strength(100f, 100f, SoulSize.None));

        // A bigger soul always enchants at least as strongly.
        check.That("a grand soul beats a lesser one",
                   power.Strength(100f, 50f, SoulSize.Grand) > power.Strength(100f, 50f, SoulSize.Lesser));

        // Convenience over an enchantment's effect.
        var fake = new FakeBackend();
        Native.Backend = fake;
        fake.SetValue(fake.Player, ActorValue.Enchanting, current: 100, baseValue: 100);
        var effect = new MagicEffectInstance(MagicEffect.From(0x300), magnitude: 50f, duration: 0);
        check.That("strength of an effect for an enchanter",
                   Near(power.Strength(effect, Game.Player, SoulSize.Grand), 75f));

        Native.Backend = null;
    }

    private static bool Near(float a, float b) => Math.Abs(a - b) < 0.01f;
}
