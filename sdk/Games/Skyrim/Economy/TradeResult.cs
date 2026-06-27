namespace Recreation.Games.Skyrim;

// The outcome of a barter transaction: whether it settled, the total gold that
// changed hands, and how many units moved. A failed trade moves nothing, so the
// caller can branch on Success without undoing anything.
public readonly struct TradeResult(bool success, int price, int count)
{
    public bool Success { get; } = success;
    public int Price { get; } = price;
    public int Count { get; } = count;

    public static TradeResult Failed { get; } = new(false, 0, 0);
}
