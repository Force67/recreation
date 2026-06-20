using System;
using System.Linq;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers Skyrim alchemy: ingredients that share an effect brew a potion carrying
// that effect (strongest magnitude, longest duration), skill scales the potency,
// and a mix with nothing in common cannot brew.
public static class AlchemyTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong effectA = 0xA00, effectB = 0xB00, effectC = 0xC00;
        const ulong ing1 = 0x100, ing2 = 0x101, ing3 = 0x102;
        fake.SetIngredientEffects(ing1, (effectA, 10, 5), (effectB, 20, 10));
        fake.SetIngredientEffects(ing2, (effectA, 15, 8), (effectC, 30, 20));
        fake.SetIngredientEffects(ing3, (effectB, 5, 30), (effectC, 25, 15));

        var alchemy = new Alchemy();
        var i1 = Ingredient.From(ing1);
        var i2 = Ingredient.From(ing2);
        var i3 = Ingredient.From(ing3);

        // Two ingredients share one effect (A): the potion carries it with the
        // stronger magnitude and the longer duration.
        BrewedPotion two = alchemy.Combine(new[] { i1, i2 }, 0);
        check.That("two-ingredient brew is valid", two.IsValid);
        check.Equal("one shared effect", 1, two.Effects.Count);
        PotionEffect a = two.Effects.Single();
        check.Equal("shared effect is A", effectA, a.Effect.Handle);
        check.Equal("takes the stronger magnitude", 15f, a.Magnitude);
        check.Equal("takes the longer duration", 8, a.Duration);

        // Three ingredients pairwise share A, B and C: all three make the potion.
        BrewedPotion three = alchemy.Combine(new[] { i1, i2, i3 }, 0);
        check.Equal("three shared effects", 3, three.Effects.Count);
        check.Equal("B takes the strongest magnitude", 20f, Effect(three, effectB).Magnitude);
        check.Equal("B takes the longest duration", 30, Effect(three, effectB).Duration);
        check.Equal("C takes the strongest magnitude", 30f, Effect(three, effectC).Magnitude);

        // Fewer than two ingredients, or no shared effect, cannot brew.
        check.That("one ingredient does not brew", !alchemy.Combine(new[] { i1 }, 0).IsValid);
        fake.SetIngredientEffects(0x200, (effectA, 10, 5));
        fake.SetIngredientEffects(0x201, (effectC, 10, 5));
        check.That("no shared effect does not brew",
                   !alchemy.Combine(new[] { Ingredient.From(0x200), Ingredient.From(0x201) }, 0).IsValid);

        // Alchemy skill scales magnitude: x1.4 at 100 skill on the default curve.
        BrewedPotion skilled = alchemy.Combine(new[] { i1, i2 }, 100);
        check.That("skill scales magnitude (15 x 1.4 ~= 21)",
                   Math.Abs(skilled.Effects.Single().Magnitude - 21f) < 0.01f);

        // The Actor overload reads the brewer's Alchemy skill.
        fake.SetValue(fake.Player, ActorValue.Alchemy, 100, 100);
        BrewedPotion byActor = alchemy.Combine(Actor.From(fake.Player), new[] { i1, i2 });
        check.That("actor brew uses the brewer's skill",
                   Math.Abs(byActor.Effects.Single().Magnitude - 21f) < 0.01f);

        Native.Backend = null;
    }

    private static PotionEffect Effect(BrewedPotion potion, ulong effectHandle) =>
        potion.Effects.Single(e => e.Effect.Handle == effectHandle);
}
