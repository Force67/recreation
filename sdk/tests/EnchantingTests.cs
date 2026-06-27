using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers enchanting: the Enchantment wrapper reads an enchantment's effects, and
// disenchanting learns it once while consuming the item.
public static class EnchantingTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        KnownEnchantments.Clear();
        EventBus.Clear();

        const ulong swordA = 0x100, swordB = 0x101, plainItem = 0x102;
        const ulong fireEnch = 0x200, fireEffect = 0x300;
        fake.SetEnchantment(swordA, fireEnch);
        fake.SetEnchantment(swordB, fireEnch);  // a second item, same enchantment
        fake.SetIngredientEffects(fireEnch, (fireEffect, 10, 0));  // the ENCH's effects

        // The wrapper reads the enchantment's effects.
        Enchantment ench = Form.From(swordA).Enchantment;
        check.That("item is enchanted", ench.Exists);
        check.Equal("enchantment handle", fireEnch, ench.Handle);
        check.Equal("enchantment exposes its effects", 1, ench.Effects.Count);
        check.Equal("effect magnitude is read", 10f, ench.Effects[0].Magnitude);

        // Disenchanting learns the enchantment and consumes the item.
        Actor player = Game.Player;
        fake.AddInventoryItem(player.Handle, swordA, 1);
        ulong learned = 0;
        using var sub = EventBus.Subscribe<EnchantmentLearned>(e => learned = e.EnchantmentHandle);

        Enchantment got = Enchanting.Disenchant(player, Form.From(swordA));
        check.Equal("disenchant returns the enchantment", fireEnch, got.Handle);
        check.That("enchantment is now known", KnownEnchantments.Knows(ench));
        check.Equal("one enchantment known", 1, KnownEnchantments.Count);
        check.Equal("item consumed", 0, player.GetItemCount(Form.From(swordA)));
        check.Equal("learn event raised", fireEnch, learned);

        // A second item with the same enchantment is consumed but not relearned.
        learned = 0;
        fake.AddInventoryItem(player.Handle, swordB, 1);
        Enchanting.Disenchant(player, Form.From(swordB));
        check.Equal("still one known", 1, KnownEnchantments.Count);
        check.Equal("no relearn event", 0UL, learned);
        check.Equal("second item consumed", 0, player.GetItemCount(Form.From(swordB)));

        // An unenchanted item yields nothing.
        Enchantment none = Enchanting.Disenchant(player, Form.From(plainItem));
        check.That("unenchanted yields no enchantment", !none.Exists);
        check.Equal("known count unchanged", 1, KnownEnchantments.Count);

        KnownEnchantments.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
