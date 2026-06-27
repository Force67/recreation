namespace Recreation.Games.Skyrim;

// A vendor the player barters with. The vendor reference holds the shop stock and
// settles in gold; prices run through BarterPricing using the buyer's Speech.
// Pure managed soft logic over inventory, gold and the item's record value, so a
// mod can stand up a working shop with no engine support beyond the items that
// already exist in the world. A transaction is all-or-nothing: it settles fully
// or moves nothing, so a half-finished trade can never leave gold unbalanced.
public sealed class Merchant
{
    private readonly ObjectReference _vendor;
    private readonly BarterPricing _pricing;

    private static Form GoldForm => Game.GetForm(SkyrimForms.Gold);

    public Merchant(ObjectReference vendor, BarterPricing? pricing = null)
    {
        _vendor = vendor;
        _pricing = pricing ?? new BarterPricing();
    }

    // The gold the merchant has on hand to buy the player's wares.
    public int Gold => _vendor.GetItemCount(GoldForm);

    // The per-unit prices for `item`, given the player's Speech and how the
    // merchant regards them (a liked merchant trades fairer). Use these to show a
    // price before committing; the trade re-derives the same number.
    public int BuyPrice(Actor player, Form item) =>
        _pricing.BuyPrice(item.GoldValue, player.GetValue(ActorValue.Speechcraft), Disposition(player));

    public int SellPrice(Actor player, Form item) =>
        _pricing.SellPrice(item.GoldValue, player.GetValue(ActorValue.Speechcraft), Disposition(player));

    // How the merchant regards the player (0..100), the disposition that nudges
    // prices. Unset relationships are neutral, so a plain shop is unaffected.
    private int Disposition(Actor player) => Relationships.Disposition(Actor.From(_vendor.Handle), player);

    // The player buys `count` of `item`: the item moves to the player and gold to
    // the merchant. Settles only if the merchant has the stock and the player the
    // gold; otherwise nothing moves and the result is Failed.
    public TradeResult PlayerBuys(Actor player, Form item, int count = 1)
    {
        int total = BuyPrice(player, item) * count;
        if (count <= 0 || total <= 0) return TradeResult.Failed;
        if (_vendor.GetItemCount(item) < count) return TradeResult.Failed;
        if (player.GetItemCount(GoldForm) < total) return TradeResult.Failed;

        _vendor.RemoveItem(item, count);
        player.AddItem(item, count);
        player.RemoveItem(GoldForm, total);
        _vendor.AddItem(GoldForm, total);
        return new TradeResult(true, total, count);
    }

    // The player sells `count` of `item`: the item moves to the merchant and gold
    // to the player. Settles only if the player has the item and the merchant can
    // cover the price from its purse.
    public TradeResult PlayerSells(Actor player, Form item, int count = 1)
    {
        int total = SellPrice(player, item) * count;
        if (count <= 0 || total <= 0) return TradeResult.Failed;
        if (player.GetItemCount(item) < count) return TradeResult.Failed;
        if (_vendor.GetItemCount(GoldForm) < total) return TradeResult.Failed;

        player.RemoveItem(item, count);
        _vendor.AddItem(item, count);
        _vendor.RemoveItem(GoldForm, total);
        player.AddItem(GoldForm, total);
        return new TradeResult(true, total, count);
    }
}
