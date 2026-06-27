using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// A player's coarse availability. Offline doubles as the "unknown" value for a
// player whose presence has not replicated yet.
public enum PlayerStatus
{
    Online,
    Away,
    Busy,
    InMenu,
    Offline,
}

// Raised on every machine when a watched player's status or activity changes,
// driven by the replicated `status`/`activity` bag keys.
public readonly struct PresenceChanged(Player player) : IGameEvent
{
    public Player Player { get; } = player;
}

// Per-player presence: a coarse status plus a free-form activity string ("In
// Whiterun", "Crafting"). Both live in the player's own state bag (an own-bag write
// a client is allowed) and replicate to everyone.
public static class Presence
{
    private const string StatusKey = "status";      // int: a PlayerStatus
    private const string ActivityKey = "activity";  // string: free-form activity

    private static readonly List<IDisposable> Subscriptions = new();

    // Observe presence keys so PresenceChanged fires wherever a value lands. Idempotent.
    internal static void Bind()
    {
        Reset();
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
    }

    internal static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
    }

    // Set the local player's presence (an own-bag write, allowed on a client).
    public static void SetStatus(PlayerStatus status) =>
        Players.Local.State.Set(StatusKey, (int)status);

    public static void SetActivity(string activity) =>
        Players.Local.State.Set(ActivityKey, activity);

    // Read any player's presence. An unset status reads as Offline (unknown).
    public static PlayerStatus StatusOf(Player player)
    {
        Value v = player.State.Get(StatusKey);
        return v.IsNone ? PlayerStatus.Offline : (PlayerStatus)v.AsInt();
    }

    public static string ActivityOf(Player player) => player.State.Get(ActivityKey).AsString();

    private static void OnBagChange(StateBagChange change)
    {
        if (change.Key != StatusKey && change.Key != ActivityKey) return;
        if (!TryPlayerId(change.Bag.Name, out uint id)) return;
        // A bare Player handle reads straight from the bag, so it carries the new
        // value even for a player not (yet) in the roster.
        EventBus.Publish(new PresenceChanged(new Player(id)));
    }

    private static bool TryPlayerId(string bagName, out uint id)
    {
        id = 0;
        const string prefix = "player:";
        return bagName.StartsWith(prefix, StringComparison.Ordinal) &&
               uint.TryParse(bagName.AsSpan(prefix.Length), out id);
    }
}
