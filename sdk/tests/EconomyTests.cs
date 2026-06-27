using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the server-authoritative economy: ledger ops, forwarded requests, jobs, shops, and client non-authority.
public static class EconomyTests
{
    private sealed class Recording : IRpcBackend
    {
        public readonly List<(RpcTarget Target, uint Peer, string Name, Value[] Args)> Emits = new();
        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args) =>
            Emits.Add((target, peer, name, args.ToArray()));
        public void Subscribe(string name) { }
    }

    public static void Run(Check check)
    {
        // --- server: a joining player is granted their starting cash ---
        Rpc.Clear();
        Rpc.Bind(new Recording());
        var changes = new List<MoneyChanged>();
        EventBus.Subscribe<MoneyChanged>(e => changes.Add(e));
        Platform.Boot(NetRole.Server);
        Economy.Bind(NetRole.Server);

        EventBus.Publish(new ClientJoined(1u));
        check.Equal("new player gets starting cash", Economy.StartingCash, Wallet.Of(1u).Cash);
        check.Equal("new player starts with no bank", 0, Wallet.Of(1u).Bank);

        // --- give / take adjust cash; bad amounts and overdrafts are rejected ---
        check.Equal("give returns Ok", TransactionResult.Ok, Economy.Give(1u, 100));
        check.Equal("give adds cash", Economy.StartingCash + 100, Wallet.Of(1u).Cash);
        check.Equal("take returns Ok", TransactionResult.Ok, Economy.Take(1u, 150));
        check.Equal("take removes cash", Economy.StartingCash - 50, Wallet.Of(1u).Cash);

        int held = Wallet.Of(1u).Cash;
        check.Equal("take beyond balance fails", TransactionResult.InsufficientFunds, Economy.Take(1u, held + 1));
        check.Equal("failed take leaves cash unchanged", held, Wallet.Of(1u).Cash);
        check.Equal("zero amount is invalid", TransactionResult.InvalidAmount, Economy.Give(1u, 0));
        check.Equal("negative amount is invalid", TransactionResult.InvalidAmount, Economy.Take(1u, -10));
        check.Equal("invalid amount leaves cash unchanged", held, Wallet.Of(1u).Cash);

        // --- deposit / withdraw move cash <-> bank ---
        check.Equal("deposit returns Ok", TransactionResult.Ok, Economy.Deposit(1u, 200));
        check.Equal("deposit lowers cash", held - 200, Wallet.Of(1u).Cash);
        check.Equal("deposit raises bank", 200, Wallet.Of(1u).Bank);
        check.Equal("withdraw returns Ok", TransactionResult.Ok, Economy.Withdraw(1u, 50));
        check.Equal("withdraw raises cash", held - 150, Wallet.Of(1u).Cash);
        check.Equal("withdraw lowers bank", 150, Wallet.Of(1u).Bank);
        check.Equal("withdraw beyond bank fails", TransactionResult.InsufficientFunds, Economy.Withdraw(1u, 1000));
        check.Equal("failed withdraw leaves bank unchanged", 150, Wallet.Of(1u).Bank);
        check.Equal("total is cash plus bank", held, Wallet.Of(1u).Total);

        // --- transfer moves money between two players; failures change nothing ---
        EventBus.Publish(new ClientJoined(2u));
        int a0 = Wallet.Of(1u).Cash, b0 = Wallet.Of(2u).Cash;
        changes.Clear();
        check.Equal("transfer returns Ok", TransactionResult.Ok, Economy.Transfer(1u, 2u, 100));
        check.Equal("transfer debits the sender", a0 - 100, Wallet.Of(1u).Cash);
        check.Equal("transfer credits the receiver", b0 + 100, Wallet.Of(2u).Cash);
        check.That("transfer raises MoneyChanged for both sides",
            changes.Exists(c => c.Player == 1u) && changes.Exists(c => c.Player == 2u));

        int a1 = Wallet.Of(1u).Cash, b1 = Wallet.Of(2u).Cash;
        check.Equal("over-balance transfer fails", TransactionResult.InsufficientFunds,
            Economy.Transfer(1u, 2u, a1 + 1));
        check.Equal("failed transfer leaves the sender unchanged", a1, Wallet.Of(1u).Cash);
        check.Equal("failed transfer leaves the receiver unchanged", b1, Wallet.Of(2u).Cash);

        // --- a client's forwarded econ:transfer is validated + applied by the host ---
        int a2 = Wallet.Of(1u).Cash, b2 = Wallet.Of(2u).Cash;
        Rpc.Dispatch("econ:transfer", 1u, false, new[] { Value.Int(2), Value.Int(75) });
        check.Equal("forwarded transfer debits the sender (the RPC peer)", a2 - 75, Wallet.Of(1u).Cash);
        check.Equal("forwarded transfer credits the target", b2 + 75, Wallet.Of(2u).Cash);

        // --- jobs: assign sets the bag, JobOf reads back, Pay credits base pay ---
        Jobs.Assign(1u, "miner");
        check.Equal("assigned job reads back", "miner", Jobs.JobOf(Players.Get(1u)!)!.Name);
        int beforePay = Wallet.Of(1u).Cash;
        check.Equal("pay returns Ok", TransactionResult.Ok, Jobs.Pay(1u));
        check.Equal("pay credits the job's base pay", beforePay + 50, Wallet.Of(1u).Cash);

        // --- shops: a buy with funds deducts the price; without funds it fails ---
        int beforeBuy = Wallet.Of(1u).Cash;
        check.Equal("buy with funds succeeds", TransactionResult.Ok, Shops.Buy(1u, "general", "potion"));
        check.Equal("buy deducts the item price", beforeBuy - 25, Wallet.Of(1u).Cash);

        Economy.Take(1u, Wallet.Of(1u).Cash);  // empty the wallet
        check.Equal("buy without funds fails", TransactionResult.InsufficientFunds,
            Shops.Buy(1u, "general", "sword"));
        check.Equal("failed buy leaves cash unchanged", 0, Wallet.Of(1u).Cash);

        // --- client: money operations are not authoritative and change nothing ---
        Economy.Reset();
        Platform.Reset();
        Rpc.Clear();
        Rpc.Bind(new Recording());
        Platform.Boot(NetRole.Client);
        Economy.Bind(NetRole.Client);
        check.Equal("client Give is not authoritative", TransactionResult.NotAuthoritative, Economy.Give(5u, 100));
        check.Equal("client Give changes nothing locally", 0, Wallet.Of(5u).Cash);

        Economy.Reset();
        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
    }
}
