using System.Collections.Generic;
using System.Linq;
using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers inventory aggregates and the loot helpers: value/weight totals, value
// ordering, and the value-per-weight pick under a carry budget.
public static class LootTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong container = 0x100, gem = 0x10, sword = 0x11, armor = 0x12, junk = 0x13;
        fake.AddInventoryItem(container, gem, 1, weight: 0f);
        fake.SetGoldValue(gem, 1000);
        fake.AddInventoryItem(container, sword, 2, weight: 10f);
        fake.SetGoldValue(sword, 100);
        fake.AddInventoryItem(container, armor, 1, weight: 30f);
        fake.SetGoldValue(armor, 200);
        fake.AddInventoryItem(container, junk, 1, weight: 5f);
        fake.SetGoldValue(junk, 5);

        var chest = ObjectReference.From(container);

        // Totals account for quantities.
        check.Equal("total value sums value times count", 1405, chest.TotalValue);
        check.Equal("total weight sums weight times count", 55f, chest.TotalWeight);

        // Value ordering.
        check.Equal("most valuable item is the gem", gem, Loot.MostValuable(chest).Handle);
        var byValue = Loot.ByValue(chest).Select(f => f.Handle).ToList();
        check.Equal("value order starts with the gem", gem, byValue[0]);
        check.Equal("value order ends with the junk", junk, byValue[^1]);

        // Smart loot under a carry budget: take the densest that fit.
        var haul = Loot.WithinWeight(chest, 35f).ToDictionary(p => p.Item.Handle, p => p.Count);
        check.That("takes the weightless gem", haul.ContainsKey(gem));
        check.Equal("takes both swords", 2, haul.GetValueOrDefault(sword));
        check.That("skips the heavy armor that would not fit", !haul.ContainsKey(armor));
        check.That("takes the light junk with the leftover budget", haul.ContainsKey(junk));

        // An empty container loots nothing.
        var empty = ObjectReference.From(0x200);
        check.Equal("empty container has no value", 0, empty.TotalValue);
        check.That("empty container has no best item", !Loot.MostValuable(empty).Exists);
        check.Equal("empty container yields no haul", 0, Loot.WithinWeight(empty, 100f).Count);

        Native.Backend = null;
    }
}
