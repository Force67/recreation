using System.Linq;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers learning an ingredient's effects by eating it: a taste consumes one,
// applies its effects to the eater and reveals the next effect in order, raising
// a discovery event the first time each is learned.
public static class IngredientDiscoveryTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        KnownIngredientEffects.Clear();
        EventBus.Clear();

        const ulong restore = 0xE00, damage = 0xE01;
        fake.SetMagicEffectInfo(restore, ActorValue.Health, detrimental: false);
        fake.SetMagicEffectInfo(damage, ActorValue.Stamina, detrimental: true);

        const ulong nirnroot = 0x500;
        fake.SetIngredientEffects(nirnroot, (restore, 5, 0), (damage, 3, 0));
        var ingredient = Ingredient.From(nirnroot);
        var player = Actor.From(fake.Player);

        // Nothing is known up front, and you cannot eat what you do not carry.
        check.Equal("no effects known initially", 0, KnownIngredientEffects.DiscoveredCount(ingredient));
        check.That("cannot eat with none on hand", !IngredientTasting.Eat(player, ingredient).Eaten);

        int discoveries = 0;
        using var sub = EventBus.Subscribe<IngredientEffectDiscovered>(_ => discoveries++);

        player.AddItem(ingredient, 3);
        fake.SetValue(fake.Player, ActorValue.Health, current: 50, baseValue: 100);
        fake.SetValue(fake.Player, ActorValue.Stamina, current: 50, baseValue: 100);

        // First taste: both effects apply (restore Health, damage Stamina) and the
        // first effect slot is revealed.
        TasteResult first = IngredientTasting.Eat(player, ingredient);
        check.That("ate one", first.Eaten);
        check.Equal("both effects took hold", 2, first.EffectsApplied);
        check.Equal("revealed the first effect", 0, first.Discovered);
        check.Equal("health restored", 55f, fake.GetCurrent(fake.Player, ActorValue.Health));
        check.Equal("stamina damaged", 47f, fake.GetCurrent(fake.Player, ActorValue.Stamina));
        check.That("first effect now known", KnownIngredientEffects.Knows(ingredient, 0));
        check.That("second effect still hidden", !KnownIngredientEffects.Knows(ingredient, 1));

        // Second taste reveals the next effect.
        TasteResult second = IngredientTasting.Eat(player, ingredient);
        check.Equal("revealed the second effect", 1, second.Discovered);
        check.Equal("both effects discovered", 2, KnownIngredientEffects.DiscoveredCount(ingredient));

        // Third taste still eats and applies, but there is nothing new to learn.
        TasteResult third = IngredientTasting.Eat(player, ingredient);
        check.That("ate the last one", third.Eaten);
        check.Equal("nothing new to discover", -1, third.Discovered);

        check.Equal("a discovery event per new effect", 2, discoveries);
        check.Equal("inventory exhausted", 0, player.GetItemCount(ingredient));
        check.Equal("known effects enumerate in order", 2, KnownIngredientEffects.Of(ingredient).Count());

        KnownIngredientEffects.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
