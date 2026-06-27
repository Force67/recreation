using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Raised on every machine when a player enters or leaves the roster. Unlike the
// engine's server-only ClientJoined/ClientLeft, these fire on clients too.
public readonly struct PlayerJoined(Player player) : IGameEvent
{
    public Player Player { get; } = player;
}

public readonly struct PlayerLeft(Player player) : IGameEvent
{
    public Player Player { get; } = player;
}

// The session's player registry. The roster is built by watching player:* state
// bags. The server marks a peer present on join and tells that peer its own id
// over a reserved RPC.
public static class Players
{
    private const string IdentityRpc = "net:you";  // server -> a client: your peer id

    private static readonly Dictionary<uint, Player> Cache = new();
    private static readonly HashSet<uint> Roster = new();
    private static readonly List<IDisposable> Subscriptions = new();

    // This machine's own player id. 0 on the host and in single-player; a client
    // learns its id from the server right after joining.
    public static uint LocalId { get; internal set; }

    // The local player, always addressable even before its presence has replicated.
    public static Player Local => Wrap(LocalId);

    // The local player's world position in engine space. This is the space the platform
    // uses (blips, spawned objects, voice), distinct from the script GetPosition space.
    public static Vector3 LocalWorldPos => new(
        Native.CallGlobal("Net", "LocalPosX", ReadOnlySpan<Value>.Empty).AsFloat(),
        Native.CallGlobal("Net", "LocalPosY", ReadOnlySpan<Value>.Empty).AsFloat(),
        Native.CallGlobal("Net", "LocalPosZ", ReadOnlySpan<Value>.Empty).AsFloat());

    // The present roster (a fresh snapshot, safe to enumerate while it mutates).
    public static IReadOnlyCollection<Player> All => Roster.Select(Wrap).ToList();

    public static int Count => Roster.Count;

    // The player with this id if it is in the roster, else null.
    public static Player? Get(uint id) => Roster.Contains(id) ? Wrap(id) : null;

    public static bool IsConnected(uint id) => Roster.Contains(id);

    // Wire the registry for a role. Idempotent; called by Platform.Boot.
    internal static void Bind(NetRole role)
    {
        Reset();
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
        switch (role)
        {
            case NetRole.Server:
                LocalId = 0;
                Subscriptions.Add(EventBus.Subscribe<ClientJoined>(e => OnPeerJoined(e.Peer)));
                // A leaving peer drops its player bag (StateBags), whose present-key
                // removal raises PlayerLeft through OnBagChange below.
                break;
            case NetRole.Client:
                Rpc.On(IdentityRpc, e => { if (e.Args.Length > 0) LocalId = (uint)e.Args[0].AsInt(); });
                break;
            case NetRole.Standalone:
                LocalId = 0;
                StateBags.Player(0).Set(Player.PresentKey, true);  // the lone local player
                break;
        }
    }

    internal static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        Cache.Clear();
        Roster.Clear();
        LocalId = 0;
    }

    // Server: a peer connected. Mark it present (replicates the roster entry to
    // everyone) and tell it its own id.
    private static void OnPeerJoined(uint peer)
    {
        StateBags.Player(peer).Set(Player.PresentKey, true);
        Rpc.ToClient(peer, IdentityRpc, Value.Int((int)peer));
    }

    // Keep the roster in step with replicated presence. Runs on every machine.
    private static void OnBagChange(StateBagChange change)
    {
        if (!TryPlayerId(change.Bag.Name, out uint id) || change.Key != Player.PresentKey) return;
        bool present = !change.Removed && change.Value.AsBool();
        if (present)
        {
            if (Roster.Add(id)) EventBus.Publish(new PlayerJoined(Wrap(id)));
        }
        else if (Roster.Remove(id))
        {
            EventBus.Publish(new PlayerLeft(Wrap(id)));
        }
    }

    private static Player Wrap(uint id)
    {
        if (!Cache.TryGetValue(id, out Player? p))
        {
            p = new Player(id);
            Cache[id] = p;
        }
        return p;
    }

    // Parse "player:<id>"; false for any other bag name (global, entity:*).
    private static bool TryPlayerId(string bagName, out uint id)
    {
        id = 0;
        const string prefix = "player:";
        return bagName.StartsWith(prefix, StringComparison.Ordinal) &&
               uint.TryParse(bagName.AsSpan(prefix.Length), out id);
    }
}
