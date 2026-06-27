using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The outcome of a money operation.
public enum TransactionResult
{
    Ok,
    InsufficientFunds,
    InvalidAmount,      // a non-positive amount
    NotAuthoritative,   // attempted on a client
}

// Raised when a player's balance changes; a transfer raises it twice, once per side.
public readonly struct MoneyChanged(uint player, int cash, int bank) : IGameEvent
{
    public uint Player { get; } = player;
    public int Cash { get; } = cash;
    public int Bank { get; } = bank;
}

// The server-authoritative ledger, the one place money is created, moved and
// destroyed. Clients get NotAuthoritative and ask the host through the Request* helpers.
public static class Economy
{
    private const string TransferRpc = "econ:transfer";  // client -> host: move my money to a peer
    private const int DefaultStartingCash = 500;

    private static NetRole _role = NetRole.Standalone;
    private static readonly List<IDisposable> Subscriptions = new();

    // Cash granted to a player the first time they join.
    public static int StartingCash { get; set; } = DefaultStartingCash;

    // Money is host-owned: only the authoritative side may mint or move it.
    internal static bool IsAuthoritative => _role != NetRole.Client;

    // Wire the economy for a role. Idempotent (re-binding resets first).
    public static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        Jobs.Bind();
        Shops.Bind(role);
        if (role == NetRole.Server)
        {
            Rpc.On(TransferRpc, OnTransferRequest);
            // Seed each joining player's wallet. Runs after Platform's roster
            // subscriber, so the player bag already exists.
            Subscriptions.Add(EventBus.Subscribe<ClientJoined>(e => GrantStartingCash(e.Peer)));
        }
    }

    // Drop subscriptions and registries and restore defaults. Called on teardown.
    public static void Reset()
    {
        foreach (IDisposable sub in Subscriptions) sub.Dispose();
        Subscriptions.Clear();
        Jobs.Reset();
        Shops.Reset();
        StartingCash = DefaultStartingCash;
        _role = NetRole.Standalone;
    }

    // Mint cash into a player's hand (a job payout, an admin grant).
    public static TransactionResult Give(uint player, int amount)
    {
        if (!IsAuthoritative) return TransactionResult.NotAuthoritative;
        if (amount <= 0) return TransactionResult.InvalidAmount;
        SetCash(player, Wallet.Of(player).Cash + amount);
        RaiseChanged(player);
        return TransactionResult.Ok;
    }

    // Remove cash from a player's hand (a purchase, a fine).
    public static TransactionResult Take(uint player, int amount)
    {
        if (!IsAuthoritative) return TransactionResult.NotAuthoritative;
        if (amount <= 0) return TransactionResult.InvalidAmount;
        int cash = Wallet.Of(player).Cash;
        if (cash < amount) return TransactionResult.InsufficientFunds;
        SetCash(player, cash - amount);
        RaiseChanged(player);
        return TransactionResult.Ok;
    }

    // Move cash into the bank.
    public static TransactionResult Deposit(uint player, int amount)
    {
        if (!IsAuthoritative) return TransactionResult.NotAuthoritative;
        if (amount <= 0) return TransactionResult.InvalidAmount;
        Wallet w = Wallet.Of(player);
        if (w.Cash < amount) return TransactionResult.InsufficientFunds;
        SetCash(player, w.Cash - amount);
        SetBank(player, w.Bank + amount);
        RaiseChanged(player);
        return TransactionResult.Ok;
    }

    // Move banked money back into the player's hand.
    public static TransactionResult Withdraw(uint player, int amount)
    {
        if (!IsAuthoritative) return TransactionResult.NotAuthoritative;
        if (amount <= 0) return TransactionResult.InvalidAmount;
        Wallet w = Wallet.Of(player);
        if (w.Bank < amount) return TransactionResult.InsufficientFunds;
        SetBank(player, w.Bank - amount);
        SetCash(player, w.Cash + amount);
        RaiseChanged(player);
        return TransactionResult.Ok;
    }

    // Move cash from one player to another in a single authoritative step.
    public static TransactionResult Transfer(uint from, uint to, int amount)
    {
        if (!IsAuthoritative) return TransactionResult.NotAuthoritative;
        if (amount <= 0) return TransactionResult.InvalidAmount;
        int fromCash = Wallet.Of(from).Cash;
        if (fromCash < amount) return TransactionResult.InsufficientFunds;
        SetCash(from, fromCash - amount);
        SetCash(to, Wallet.Of(to).Cash + amount);
        RaiseChanged(from);
        RaiseChanged(to);
        return TransactionResult.Ok;
    }

    // Client helper: ask the host to transfer the local player's money to a peer.
    public static void RequestTransfer(uint to, int amount) =>
        Rpc.Emit(TransferRpc, Value.Int((int)to), Value.Int(amount));

    // Host: a client asked to send money. The sender is the authoritative `from`.
    private static void OnTransferRequest(RpcEvent e)
    {
        if (e.Args.Length < 2) return;
        Transfer(e.Sender, (uint)e.Args[0].AsInt(), e.Args[1].AsInt());
    }

    private static void GrantStartingCash(uint player)
    {
        SetCash(player, StartingCash);
        RaiseChanged(player);
    }

    private static void SetCash(uint player, int cash) =>
        StateBags.Player(player).Set(Wallet.CashKey, cash);

    private static void SetBank(uint player, int bank) =>
        StateBags.Player(player).Set(Wallet.BankKey, bank);

    // Announce the player's balance, read back from the bag.
    private static void RaiseChanged(uint player)
    {
        Wallet w = Wallet.Of(player);
        EventBus.Publish(new MoneyChanged(player, w.Cash, w.Bank));
    }
}
