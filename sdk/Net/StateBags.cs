using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The registry and replication authority for state bags. It owns every bag, routes
// writes by role, and keeps clients in sync with the host:
//
//   * Server     - a set applies locally and broadcasts the new value. Inbound
//                  client write requests pass a validator before apply + rebroadcast.
//   * Client     - a set is sent to the server as a request; the change becomes
//                  visible only when the server's authoritative echo arrives.
//   * Standalone - a set just applies locally.
//
// Replication rides the existing scripting RPC channel (three reserved names). A
// joining player is sent the full current state.
public static class StateBags
{
    // Reserved RPC names. Namespaced so they never collide with a mod's own RPCs.
    private const string SetRpc = "sb:set";   // server -> clients: a value changed
    private const string ReqRpc = "sb:req";   // client -> server: please change a value
    private const string DropRpc = "sb:drop"; // server -> clients: a whole bag went away

    public const string GlobalName = "global";

    private static readonly Dictionary<string, StateBag> Bags = new();
    private static NetRole _role = NetRole.Standalone;
    private static readonly List<IDisposable> Subscriptions = new();
    // Observers of every change across every bag (player roster, scoreboard).
    private static readonly List<Action<StateBagChange>> GlobalObservers = new();

    // Gate for inbound client writes; returns true to accept. Default lets a client
    // write only its own player bag; a mod widens or tightens it.
    private static Func<uint, string, string, Value, bool> _writeValidator = DefaultValidator;

    // The world-scoped bag every player shares. Server-authoritative.
    public static StateBag Global => Bag(GlobalName);

    // Bags attached to a player (by peer id) and to a networked entity (by net id).
    // Naming is conventional so both sides resolve the same bag.
    public static StateBag Player(uint peer) => Bag($"player:{peer}");
    public static StateBag Entity(ulong netId) => Bag($"entity:{netId}");

    public static NetRole Role => _role;

    // Get-or-create a bag by name. Created lazily, so referencing player:7 before any
    // state arrives still gets a live bag.
    public static StateBag Bag(string name)
    {
        if (!Bags.TryGetValue(name, out StateBag? bag))
        {
            bag = new StateBag(name);
            Bags[name] = bag;
        }
        return bag;
    }

    // The bag if it exists, else null. For read-only inspection without creating one.
    public static StateBag? Find(string name) => Bags.TryGetValue(name, out StateBag? bag) ? bag : null;

    public static IReadOnlyCollection<StateBag> All => Bags.Values;

    // Replace the client-write gate. The validator sees sender peer, bag, key, value.
    public static void SetClientWriteValidator(Func<uint, string, string, Value, bool> validator) =>
        _writeValidator = validator ?? DefaultValidator;

    // Observe every change to every bag (systems that span bags). The token detaches.
    public static IDisposable OnAnyChange(Action<StateBagChange> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        GlobalObservers.Add(handler);
        return new Unsubscriber(() => GlobalObservers.Remove(handler));
    }

    // Fired by StateBag.ApplyLocal for each real change. Snapshotted so an observer
    // may (un)subscribe mid-dispatch.
    internal static void RaiseGlobalChange(in StateBagChange change)
    {
        if (GlobalObservers.Count == 0) return;
        Action<StateBagChange>[] snapshot = GlobalObservers.ToArray();
        foreach (Action<StateBagChange> h in snapshot)
        {
            try { h(change); }
            catch (Exception ex) { Console.Error.WriteLine($"[statebag] observer threw: {ex.Message}"); }
        }
    }

    // Wire the layer for a role. Idempotent (re-binding resets first). Called by Platform.Boot.
    internal static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        switch (role)
        {
            case NetRole.Server:
                Rpc.On(ReqRpc, OnClientWrite);
                // Sync a player on join, and again once its UGC has streamed (covers
                // sessions with and without mod streaming); the second pass no-ops if unchanged.
                Track(EventBus.Subscribe<ClientJoined>(e => SyncTo(e.Peer)));
                Track(EventBus.Subscribe<ClientAssetsReady>(e => SyncTo(e.Peer)));
                Track(EventBus.Subscribe<ClientLeft>(e => DropPlayerBag(e.Peer)));
                break;
            case NetRole.Client:
                Rpc.On(SetRpc, OnServerSet);
                Rpc.On(DropRpc, OnServerDrop);
                break;
            case NetRole.Standalone:
                break;  // no network
        }
    }

    // Drop every bag, handler and subscription. Called on session teardown.
    public static void Reset()
    {
        foreach (IDisposable sub in Subscriptions) sub.Dispose();
        Subscriptions.Clear();
        Bags.Clear();
        GlobalObservers.Clear();
        _role = NetRole.Standalone;
        _writeValidator = DefaultValidator;
    }

    private static void Track(IDisposable sub) => Subscriptions.Add(sub);

    // The Set path from StateBag.Set, branched by role.
    internal static void SetFrom(StateBag bag, string key, Value value, bool replicated)
    {
        if (!replicated || _role == NetRole.Standalone)
        {
            bag.ApplyLocal(key, value, fromServer: false);
            return;
        }
        if (_role == NetRole.Server)
        {
            ApplyAndBroadcast(bag, key, value);
            return;
        }
        // Client: ask the server. The change lands when the authoritative echo (sb:set)
        // comes back, so the client never diverges optimistically.
        Rpc.Emit(ReqRpc, Value.String(bag.Name), Value.String(key), value);
    }

    // Server: apply locally and push the new value to every client.
    private static void ApplyAndBroadcast(StateBag bag, string key, Value value)
    {
        bag.ApplyLocal(key, value, fromServer: false);
        Rpc.Broadcast(SetRpc, Value.String(bag.Name), Value.String(key), value);
    }

    // Server: a client asked to change a value. Validate, then treat it as a normal
    // authoritative set if accepted.
    private static void OnClientWrite(RpcEvent e)
    {
        if (e.Args.Length < 3) return;
        string name = e.Args[0].AsString();
        string key = e.Args[1].AsString();
        Value value = e.Args[2];
        if (!_writeValidator(e.Sender, name, key, value)) return;
        ApplyAndBroadcast(Bag(name), key, value);
    }

    // Client: the server published a value. Apply it as authoritative truth.
    private static void OnServerSet(RpcEvent e)
    {
        if (e.Args.Length < 3) return;
        Bag(e.Args[0].AsString()).ApplyLocal(e.Args[1].AsString(), e.Args[2], fromServer: true);
    }

    // Client: the server dropped a whole bag (e.g. a player left). Clear every key
    // so per-key handlers see the removals, then forget the bag.
    private static void OnServerDrop(RpcEvent e)
    {
        if (e.Args.Length < 1) return;
        DropBagLocal(e.Args[0].AsString(), fromServer: true);
    }

    // Server: send a joining peer the entire current state so it starts in sync.
    private static void SyncTo(uint peer)
    {
        foreach (StateBag bag in Bags.Values)
            foreach (KeyValuePair<string, Value> kv in bag.Snapshot())
                Rpc.ToClient(peer, SetRpc, Value.String(bag.Name), Value.String(kv.Key), kv.Value);
    }

    // Server: a player left. Drop its player bag locally and tell clients to as well.
    private static void DropPlayerBag(uint peer)
    {
        string name = $"player:{peer}";
        if (!Bags.ContainsKey(name)) return;
        DropBagLocal(name, fromServer: false);
        Rpc.Broadcast(DropRpc, Value.String(name));
    }

    // Clear and forget a bag, firing per-key removal changes first so handlers can
    // react (drop a player's blip, clear their scoreboard row).
    private static void DropBagLocal(string name, bool fromServer)
    {
        if (!Bags.TryGetValue(name, out StateBag? bag)) return;
        foreach (KeyValuePair<string, Value> kv in bag.Snapshot())
            bag.ApplyLocal(kv.Key, Value.None, fromServer);
        Bags.Remove(name);
    }

    // Safe default: a client may write only its own player bag; everything else is
    // server-owned unless a mod overrides via SetClientWriteValidator.
    private static bool DefaultValidator(uint peer, string bag, string key, Value value) =>
        bag == $"player:{peer}";
}
