namespace Recreation.Net;

// A read-only view of a player's money, projected over their replicated state bag.
// Mutation goes through Economy.
public readonly struct Wallet
{
    // The player-bag keys money lives under. Internal so only Economy writes them.
    internal const string CashKey = "cash";
    internal const string BankKey = "bank";

    public uint Player { get; }

    private Wallet(uint player) => Player = player;

    public static Wallet Of(uint player) => new(player);
    public static Wallet Of(Player player) => new(player.Id);

    // On-hand money; an unset key reads 0.
    public int Cash => StateBags.Player(Player).Get(CashKey).AsInt();

    // Banked money, moved to and from cash via Economy.Deposit / Economy.Withdraw.
    public int Bank => StateBags.Player(Player).Get(BankKey).AsInt();

    public int Total => Cash + Bank;
}
