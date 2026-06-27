using System;

namespace Recreation.Games.Skyrim;

// Skyrim barter pricing: the player pays above an item's value to buy and gets
// back below it to sell, the spread closing as their Speech rises (a master
// haggler trades near fair value). Pure math with no inventory side effects, so a
// price preview and the settled trade share one formula, and a mod retunes the
// economy by constructing its own with different multipliers.
public sealed class BarterPricing
{
    // Price multiplier at 0 Speech: buying pays Buy x value, selling returns
    // Sell x value. The defaults give the familiar "pay 1.5x, get 0.5x" feel.
    public float BaseBuyMultiplier { get; init; } = 1.5f;
    public float BaseSellMultiplier { get; init; } = 0.5f;

    // How much one point of Speech narrows the spread toward fair value (1.0).
    // With the defaults, 100 Speech reaches a fair trade on both sides.
    public float ReliefPerSpeech { get; init; } = 0.005f;

    // How much disposition counts as Speech: a `disposition` above the neutral 50
    // trades like extra Speech (a friend's discount), below it like a penalty. So a
    // liked merchant and a high Speech reach a fair trade together.
    public float DispositionAsSpeech { get; init; } = 1f;

    // What the player pays for one unit worth `value` at `speech` Speech, with the
    // merchant's `disposition` (0..100, 50 neutral) folded in. A worthless item
    // (value <= 0) has no price.
    public int BuyPrice(int value, float speech, int disposition = 50)
    {
        if (value <= 0) return 0;
        float mult = Mathf.Clamp(BaseBuyMultiplier - ReliefPerSpeech * Effective(speech, disposition),
                                 1f, BaseBuyMultiplier);
        return Math.Max(1, (int)MathF.Ceiling(value * mult));
    }

    // What the player receives for selling one unit worth `value`.
    public int SellPrice(int value, float speech, int disposition = 50)
    {
        if (value <= 0) return 0;
        float mult = Mathf.Clamp(BaseSellMultiplier + ReliefPerSpeech * Effective(speech, disposition),
                                 BaseSellMultiplier, 1f);
        return Math.Max(1, (int)MathF.Floor(value * mult));
    }

    private float Effective(float speech, int disposition) =>
        speech + (disposition - 50) * DispositionAsSpeech;
}
