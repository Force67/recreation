using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the Skyrim bartering soft logic: the price curve over Speech, and that a
// settled trade moves the item and gold both ways while a rejected one moves
// nothing.
public static class BarteringTests
{
    public static void Run(Check check)
    {
        PricingCurve(check);
        Transactions(check);
    }

    private static void PricingCurve(Check check)
    {
        var pricing = new BarterPricing();

        // At 0 Speech the spread is widest: pay 1.5x, get back 0.5x.
        check.Equal("buy price at 0 speech", 150, pricing.BuyPrice(100, 0));
        check.Equal("sell price at 0 speech", 50, pricing.SellPrice(100, 0));

        // The defaults reach a fair trade at 100 Speech.
        check.Equal("buy price at 100 speech", 100, pricing.BuyPrice(100, 100));
        check.Equal("sell price at 100 speech", 100, pricing.SellPrice(100, 100));

        // Buying never drops below, nor selling above, fair value, whatever Speech.
        check.Equal("buy never beats fair value", 100, pricing.BuyPrice(100, 500));
        check.Equal("sell never beats fair value", 100, pricing.SellPrice(100, 500));

        // A worthless item has no price on either side.
        check.Equal("worthless item has no buy price", 0, pricing.BuyPrice(0, 50));
        check.Equal("worthless item has no sell price", 0, pricing.SellPrice(0, 50));

        // Disposition trades like Speech: a liked merchant (above the neutral 50)
        // is cheaper to buy from, a disliked one dearer; neutral changes nothing.
        check.Equal("neutral disposition is unchanged", 150, pricing.BuyPrice(100, 0, disposition: 50));
        check.Equal("a friendly merchant is cheaper", 125, pricing.BuyPrice(100, 0, disposition: 100));
        check.Equal("a friendly merchant pays more", 75, pricing.SellPrice(100, 0, disposition: 100));
        check.Equal("a hostile merchant stays at the worst markup", 150,
                    pricing.BuyPrice(100, 0, disposition: 0));
    }

    private static void Transactions(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong vendorHandle = 0x200;
        const ulong itemHandle = 0x300;
        const ulong gold = SkyrimForms.Gold;  // Game.GetForm maps the id to this handle

        var player = Actor.From(fake.Player);
        var vendor = ObjectReference.From(vendorHandle);
        var item = Form.From(itemHandle);
        var goldForm = Form.From(gold);
        var merchant = new Merchant(vendor);

        fake.SetValue(fake.Player, ActorValue.Speechcraft, 0, 0);  // worst prices
        fake.SetGoldValue(itemHandle, 100);
        fake.AddInventoryItem(vendorHandle, itemHandle, 5);
        fake.AddInventoryItem(vendorHandle, gold, 1000);
        fake.AddInventoryItem(fake.Player, gold, 1000);

        // The player buys one: the item comes in, 150 gold goes out, and the
        // merchant's stock and purse mirror it.
        TradeResult buy = merchant.PlayerBuys(player, item);
        check.That("buy settles", buy.Success);
        check.Equal("buy price charged", 150, buy.Price);
        check.Equal("player received the item", 1, player.GetItemCount(item));
        check.Equal("player paid from their purse", 850, player.GetItemCount(goldForm));
        check.Equal("merchant stock decremented", 4, vendor.GetItemCount(item));
        check.Equal("merchant purse grew", 1150, vendor.GetItemCount(goldForm));

        // Too little gold: nothing moves.
        var pauper = Actor.From(0x15);
        fake.SetValue(0x15, ActorValue.Speechcraft, 0, 0);
        fake.AddInventoryItem(0x15, gold, 100);
        TradeResult broke = merchant.PlayerBuys(pauper, item);
        check.That("buy rejected when short on gold", !broke.Success);
        check.Equal("pauper kept their gold", 100, pauper.GetItemCount(goldForm));
        check.Equal("merchant stock untouched", 4, vendor.GetItemCount(item));

        // Out of stock: nothing moves.
        const ulong rareHandle = 0x301;
        fake.SetGoldValue(rareHandle, 100);
        TradeResult oos = merchant.PlayerBuys(player, Form.From(rareHandle));
        check.That("buy rejected when out of stock", !oos.Success);

        // The player sells two of a stack back: items go to the merchant, gold (50
        // each at 0 Speech) comes back. The player holds three going in.
        fake.AddInventoryItem(fake.Player, itemHandle, 2);  // 1 bought + 2 = 3 held
        TradeResult sell = merchant.PlayerSells(player, item, 2);
        check.That("sell settles", sell.Success);
        check.Equal("sell price paid", 100, sell.Price);                  // 2 x 50
        check.Equal("player handed over two", 1, player.GetItemCount(item));      // 3 - 2
        check.Equal("merchant took the items", 6, vendor.GetItemCount(item));     // 4 + 2
        check.Equal("player gold went up", 950, player.GetItemCount(goldForm));   // 850 + 100
        check.Equal("merchant paid from its purse", 1050, vendor.GetItemCount(goldForm));  // 1150 - 100

        // The merchant cannot cover a huge sale: nothing moves.
        const ulong gemHandle = 0x302;
        fake.SetGoldValue(gemHandle, 100000);
        fake.AddInventoryItem(fake.Player, gemHandle, 1);
        TradeResult tooBig = merchant.PlayerSells(player, Form.From(gemHandle));
        check.That("sell rejected when merchant is broke", !tooBig.Success);
        check.Equal("player kept the unsold item", 1, player.GetItemCount(Form.From(gemHandle)));

        // The merchant's disposition toward the player feeds the price: an ally
        // charges less than a neutral acquaintance.
        Relationships.Clear();
        Relationships.Set(Actor.From(vendorHandle), player, RelationshipRank.Ally);
        int friendlyPrice = merchant.BuyPrice(player, item);
        Relationships.Set(Actor.From(vendorHandle), player, RelationshipRank.Acquaintance);
        check.That("a friendly merchant charges less", friendlyPrice < merchant.BuyPrice(player, item));
        Relationships.Clear();

        Native.Backend = null;
    }
}
