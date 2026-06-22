using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised when the player's credit balance changes.
public readonly struct CreditsChanged(ulong actorHandle, long balance) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public long Balance { get; } = balance;
}

// Starfield's credit economy as managed soft logic, the counterpart to Skyrim's
// bartering: a tracked balance per actor and buy/sell transactions that move
// credits against the actor's inventory. A vendor interaction calls Buy/Sell; the
// registry only holds the balance and enforces that a purchase is affordable and a
// sale is backed by the goods. Persistent player state, so it outlives the
// mod-host lifecycle; tests clear it.
public static class Economy
{
    private static readonly Dictionary<ulong, long> Balance = new();

    public static long Credits(Actor actor) => Balance.GetValueOrDefault(actor.Handle);

    // Adds credits (a negative amount removes them), never dropping below zero, and
    // announces the new balance.
    public static void Add(Actor actor, long amount)
    {
        long balance = System.Math.Max(0, Credits(actor) + amount);
        Balance[actor.Handle] = balance;
        EventBus.Publish(new CreditsChanged(actor.Handle, balance));
    }

    // Spends credits when the actor can afford it. Returns whether it was spent.
    public static bool Spend(Actor actor, long amount)
    {
        if (amount <= 0 || Credits(actor) < amount) return false;
        Add(actor, -amount);
        return true;
    }

    // Buys `count` of `item` at `unitPrice` each: spends the total then adds the
    // goods. Returns false when the actor cannot afford it.
    public static bool Buy(Actor actor, uint item, long unitPrice, int count = 1)
    {
        if (count <= 0 || unitPrice < 0) return false;
        if (!Spend(actor, unitPrice * count)) return false;
        actor.AddItem(Game.GetForm(item), count);
        return true;
    }

    // Sells `count` of `item` at `unitPrice` each: removes the goods then credits
    // the proceeds. Returns false when the actor lacks the items.
    public static bool Sell(Actor actor, uint item, long unitPrice, int count = 1)
    {
        if (count <= 0 || unitPrice < 0) return false;
        if (actor.GetItemCount(Game.GetForm(item)) < count) return false;
        actor.RemoveItem(Game.GetForm(item), count);
        Add(actor, unitPrice * count);
        return true;
    }

    public static void Clear() => Balance.Clear();
}
