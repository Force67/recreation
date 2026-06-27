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

        Value(check, fake, alchemy);
        Native.Backend = null;
    }

    // The brew's worth and whether it is a potion or a poison, both read off the
    // shared effects' base costs and which one is the most valuable.
    private static void Value(Check check, FakeBackend fake, Alchemy alchemy)
    {
        // The unit curve on its own: base cost times the magnitude/duration factor,
        // with no base cost meaning no worth.
        check.Equal("unit effect at magnitude 1", 10, AlchemyValue.OfEffect(10f, 1f, 0));
        check.Equal("a 10s duration carries no extra factor", 10, AlchemyValue.OfEffect(10f, 1f, 10));
        check.Equal("longer duration raises worth", 21, AlchemyValue.OfEffect(10f, 1f, 20));
        check.Equal("no base cost is worthless", 0, AlchemyValue.OfEffect(0f, 100f, 100));

        // A beneficial Restore-Health effect and a harmful Damage-Health one, the
        // latter worth more, so a brew sharing both is a poison named by it.
        const ulong cure = 0xA10, harm = 0xA11;
        fake.SetMagicEffectInfo(cure, "Health", detrimental: false, baseCost: 2f);
        fake.SetMagicEffectInfo(harm, "Health", detrimental: true, baseCost: 4f);
        const ulong ingP = 0x310, ingQ = 0x311;
        fake.SetIngredientEffects(ingP, (cure, 10, 0), (harm, 7, 0));
        fake.SetIngredientEffects(ingQ, (cure, 8, 0), (harm, 5, 0));

        BrewedPotion brew = alchemy.Combine(new[] { Ingredient.From(ingP), Ingredient.From(ingQ) }, 0);
        check.Equal("brew shares both effects", 2, brew.Effects.Count);
        check.Equal("primary effect is the most valuable (harm)", harm, brew.PrimaryEffect.Effect.Handle);
        check.That("harmful primary makes it a poison", brew.IsPoison);
        check.That("a poison is not a potion", !brew.IsPotion);
        int expected = AlchemyValue.OfEffect(2f, 10, 0) + AlchemyValue.OfEffect(4f, 7, 0);
        check.Equal("gold value sums the effects' worth", expected, brew.GoldValue);

        // Drop the harmful effect and the same mix brews a (beneficial) potion.
        fake.SetIngredientEffects(ingP, (cure, 10, 0));
        fake.SetIngredientEffects(ingQ, (cure, 8, 0));
        BrewedPotion potion = alchemy.Combine(new[] { Ingredient.From(ingP), Ingredient.From(ingQ) }, 0);
        check.That("beneficial primary makes it a potion", potion.IsPotion);
        check.That("a potion is not a poison", !potion.IsPoison);
    }

    private static PotionEffect Effect(BrewedPotion potion, ulong effectHandle) =>
        potion.Effects.Single(e => e.Effect.Handle == effectHandle);
}
