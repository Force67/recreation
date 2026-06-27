using System.Collections.Generic;
using System.Linq;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers carry-weight management: measuring the overflow and shedding the least
// worthwhile items to get back within capacity, keeping the best loot.
public static class CarryWeightTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong gem = 0x10, sword = 0x11, armor = 0x12, junk = 0x13;
        ulong player = fake.Player;
        fake.SetValue(player, ActorValue.CarryWeight, current: 50, baseValue: 50);
        fake.AddInventoryItem(player, gem, 1, weight: 0f);
        fake.SetGoldValue(gem, 1000);
        fake.AddInventoryItem(player, sword, 2, weight: 10f);
        fake.SetGoldValue(sword, 100);
        fake.AddInventoryItem(player, armor, 1, weight: 30f);
        fake.SetGoldValue(armor, 200);
        fake.AddInventoryItem(player, junk, 1, weight: 5f);
        fake.SetGoldValue(junk, 5);

        Actor p = Game.Player;

        // Measurement: 55 carried against a 50 capacity is 5 over.
        check.Equal("capacity is the carry-weight value", 50f, CarryWeight.Capacity(p));
        check.Equal("total weight is the inventory sum", 55f, p.TotalWeight);
        check.Equal("overflow is the amount over capacity", 5f, CarryWeight.Overflow(p));
        check.That("the actor is over-encumbered", CarryWeight.IsOverEncumbered(p));

        // Shedding drops the worthless junk and keeps the valuables.
        Dictionary<ulong, int> shed =
            CarryWeight.ShedExcess(p).ToDictionary(s => s.Item.Handle, s => s.Count);
        check.That("the worthless junk is shed", shed.ContainsKey(junk));
        check.That("the gem is kept", !shed.ContainsKey(gem));
        check.That("the swords are kept", !shed.ContainsKey(sword));
        check.That("the armor is kept", !shed.ContainsKey(armor));
        check.Equal("junk left the inventory", 0, p.GetItemCount(Form.From(junk)));
        check.Equal("the gem stayed", 1, p.GetItemCount(Form.From(gem)));

        // Back within capacity.
        check.That("no longer over-encumbered", !CarryWeight.IsOverEncumbered(p));
        check.Equal("shedding again drops nothing", 0, CarryWeight.ShedExcess(p).Count);

        Native.Backend = null;
    }
}
