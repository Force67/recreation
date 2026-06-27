using System;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers soul gems: reading the held soul and capacity, the filled/full state, and
// powering an enchantment with a gem's soul.
public static class SoulGemTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong grandFilled = 0x100, pettyEmpty = 0x101, partial = 0x102;
        fake.SetSoulGem(grandFilled, soul: 5, capacity: 5);  // a filled grand gem
        fake.SetSoulGem(pettyEmpty, soul: 0, capacity: 1);   // an empty petty gem
        fake.SetSoulGem(partial, soul: 2, capacity: 5);      // a lesser soul in a grand gem

        SoulGem grand = SoulGem.From(grandFilled);
        check.Equal("the grand gem holds a grand soul", SoulSize.Grand, grand.Soul);
        check.Equal("the grand gem's capacity is grand", SoulSize.Grand, grand.Capacity);
        check.That("the grand gem is filled", grand.IsFilled);
        check.That("the grand gem is full", grand.IsFull);

        SoulGem petty = SoulGem.From(pettyEmpty);
        check.Equal("the petty gem is empty", SoulSize.None, petty.Soul);
        check.That("the empty gem is not filled", !petty.IsFilled);

        SoulGem under = SoulGem.From(partial);
        check.Equal("the partial gem holds a lesser soul", SoulSize.Lesser, under.Soul);
        check.That("a partly-filled gem is filled but not full", under.IsFilled && !under.IsFull);

        // The gem's soul powers an enchantment's strength.
        fake.SetValue(fake.Player, ActorValue.Enchanting, current: 100, baseValue: 100);
        var effect = new MagicEffectInstance(MagicEffect.From(0x300), magnitude: 50f, duration: 0);
        var power = new EnchantingPower();
        check.That("a grand soul enchants at full strength",
                   Near(power.Strength(effect, Game.Player, grand), 75f));     // 50 * 1.5 * 1.0
        check.That("a lesser soul enchants weaker",
                   power.Strength(effect, Game.Player, under) < power.Strength(effect, Game.Player, grand));

        Native.Backend = null;
    }

    private static bool Near(float a, float b) => Math.Abs(a - b) < 0.01f;
}
