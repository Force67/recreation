using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Starfield credit economy: credits accrue and spend without going
// negative, a purchase needs the funds and yields the goods, and a sale needs the
// goods and pays the proceeds.
public static class StarfieldEconomyTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        Economy.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        long lastBalance = -1;
        using var sub = EventBus.Subscribe<CreditsChanged>(e => lastBalance = e.Balance);

        check.Equal("starts with no credits", 0L, Economy.Credits(player));

        Economy.Add(player, 500);
        check.Equal("credits accrue", 500L, Economy.Credits(player));
        check.Equal("a balance event fired", 500L, lastBalance);

        // Cannot overspend; the balance holds.
        check.That("cannot spend more than held", !Economy.Spend(player, 600));
        check.Equal("balance unchanged after a failed spend", 500L, Economy.Credits(player));

        // A purchase needs the funds and yields the goods.
        const uint medpack = 0xB0;
        check.That("cannot buy without enough credits", !Economy.Buy(player, medpack, unitPrice: 600));
        check.That("buys when affordable", Economy.Buy(player, medpack, unitPrice: 120, count: 2));
        check.Equal("credits paid for the purchase", 260L, Economy.Credits(player));
        check.Equal("the goods arrived", 2, player.GetItemCount(Game.GetForm(medpack)));

        // A sale needs the goods and pays the proceeds.
        const uint ore = 0xB1;
        check.That("cannot sell goods not held", !Economy.Sell(player, ore, unitPrice: 50));
        fake.AddInventoryItem(player.Handle, Game.GetForm(ore).Handle, 3);
        check.That("sells held goods", Economy.Sell(player, ore, unitPrice: 50, count: 3));
        check.Equal("proceeds credited", 410L, Economy.Credits(player));
        check.Equal("the goods left the pack", 0, player.GetItemCount(Game.GetForm(ore)));

        // Credits never go negative even on a large removal.
        Economy.Add(player, -100000);
        check.Equal("credits floor at zero", 0L, Economy.Credits(player));

        Economy.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
