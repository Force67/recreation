using System.Collections.Generic;
using System.Linq;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// The enchantments the player has learned by disenchanting: a modding-facing
// registry of known ENCH forms, the basis for an enchanting menu (query what is
// known, enumerate it, learn more). Persistent player knowledge, so it is not
// tied to the mod-host lifecycle; tests clear it explicitly.
public static class KnownEnchantments
{
    private static readonly HashSet<ulong> Known = new();

    // Records an enchantment as known; returns true if it was not known before.
    public static bool Learn(Enchantment enchantment) =>
        enchantment.Exists && Known.Add(enchantment.Handle);

    public static bool Knows(Form enchantment) => Known.Contains(enchantment.Handle);

    public static int Count => Known.Count;

    public static IEnumerable<Enchantment> All => Known.Select(Recreation.Enchantment.From);

    public static void Clear() => Known.Clear();
}

// Raised when the player learns a new enchantment by disenchanting an item.
public readonly struct EnchantmentLearned(ulong enchantmentHandle) : IGameEvent
{
    public ulong EnchantmentHandle { get; } = enchantmentHandle;

    public Enchantment Enchantment => Recreation.Enchantment.From(EnchantmentHandle);
}

// Disenchanting, the Skyrim mechanic: breaking an enchanted item down to learn its
// enchantment. Soft logic over the item's record data, the inventory and the
// known-enchantments registry. Applying a learned enchantment to a fresh item
// needs engine support for dynamic forms, so it is not part of this.
public static class Enchanting
{
    // Disenchants `item` from `who`: consumes one and learns its enchantment if it
    // is new. Returns the enchantment (a null Enchantment if the item carried none).
    public static Enchantment Disenchant(Actor who, Form item)
    {
        Enchantment enchantment = item.Enchantment;
        if (!enchantment.Exists) return enchantment;

        who.RemoveItem(item, 1);
        if (KnownEnchantments.Learn(enchantment))
            EventBus.Publish(new EnchantmentLearned(enchantment.Handle));
        return enchantment;
    }
}
