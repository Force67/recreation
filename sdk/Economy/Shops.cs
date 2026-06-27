using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The shop registry and the server-authoritative buy path. Shops are shared static
// data; a client asks to buy through RequestBuy and the host applies it.
public static class Shops
{
    private const string BuyRpc = "econ:buy";  // client -> host: I want to buy an item

    private static readonly Dictionary<string, Shop> Registry = new(StringComparer.OrdinalIgnoreCase);

    public static void Register(Shop shop) => Registry[shop.Name] = shop;

    public static Shop? Get(string name) => Registry.TryGetValue(name, out Shop? shop) ? shop : null;

    public static IReadOnlyCollection<Shop> All => Registry.Values;

    // Server: charge a player for an item through the ledger. An unknown shop or
    // item returns InvalidAmount.
    public static TransactionResult Buy(uint player, string shop, string item)
    {
        int? price = Get(shop)?.PriceOf(item);
        if (price is null) return TransactionResult.InvalidAmount;
        return Economy.Take(player, price.Value);
    }

    // Client helper: ask the host to buy an item for the local player.
    public static void RequestBuy(string shop, string item) =>
        Rpc.Emit(BuyRpc, Value.String(shop), Value.String(item));

    // Host: a client asked to buy. The sender is the buyer.
    private static void OnBuyRequest(RpcEvent e)
    {
        if (e.Args.Length < 2) return;
        Buy(e.Sender, e.Args[0].AsString(), e.Args[1].AsString());
    }

    // Bring up the registry with a sample shop, and on the host the buy handler.
    internal static void Bind(NetRole role)
    {
        Register(new Shop("general", ("bread", 5), ("potion", 25), ("sword", 300)));
        if (role == NetRole.Server) Rpc.On(BuyRpc, OnBuyRequest);
    }

    internal static void Reset() => Registry.Clear();
}
