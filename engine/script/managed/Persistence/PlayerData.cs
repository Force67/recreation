using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Ties a player's durable save to its replicated state bag. A mod declares which keys
// to keep with Persist; PlayerData copies those between the bag and a store, namespaced
// per player. Host-authoritative: only the server saves. Automatic via Bind (save on
// leave, load on join); Save/Load are also public for manual checkpoints.
public static class PlayerData
{
    // The default store keeps saves in memory; a host swaps in a JsonFileKvStore.
    private static IKvStore _store = new MemoryKvStore();

    // The state-bag keys that get saved; transient bag entries are not persisted.
    private static readonly List<string> PersistedKeys = new();

    private static Func<Player, string> _identity = DefaultIdentity;

    private static readonly List<IDisposable> Subscriptions = new();

    // Saving must beat StateBags dropping a leaving player's bag (same ClientLeft event),
    // so the leave handler runs at high priority while the bag still holds data.
    private const int SavePriority = 1000;

    public static IKvStore Store => _store;

    // Choose the backing store (a host points this at a JsonFileKvStore at boot).
    public static void UseStore(IKvStore store) =>
        _store = store ?? throw new ArgumentNullException(nameof(store));

    // Declare state-bag keys to persist. Additive and idempotent.
    public static void Persist(params string[] keys)
    {
        foreach (string key in keys)
            if (!PersistedKeys.Contains(key)) PersistedKeys.Add(key);
    }

    // Override how a player maps to a save slot (e.g. an account id from an auth
    // mod). Passing null restores the default name-or-id rule.
    public static void SetIdentityResolver(Func<Player, string> resolver) =>
        _identity = resolver ?? DefaultIdentity;

    // The stable save identity for a player. The default prefers the chosen name (peer
    // ids are per-session and would scatter saves), falling back to "player.<id>".
    public static string IdentityOf(Player player) => _identity(player);

    private static string DefaultIdentity(Player player)
    {
        string name = player.State.Get(Player.NameKey).AsString();
        return string.IsNullOrEmpty(name) ? $"player.{player.Id}" : name;
    }

    // Copy the persisted keys from a player's bag into the store, then flush. A key
    // that is unset on the bag is deleted from the save so it does not linger.
    public static void Save(Player? player)
    {
        if (player == null) return;
        IKvStore slot = _store.Scope(IdentityOf(player));
        StateBag bag = player.State;
        foreach (string key in PersistedKeys)
        {
            Value v = bag.Get(key);
            if (v.IsNone) slot.Delete(key);
            else slot.Set(key, v);
        }
        _store.Flush();
    }

    // Copy the persisted keys from the store back onto a player's bag. Only keys the
    // save actually holds are written, so a first-time player is left untouched.
    public static void Load(Player? player)
    {
        if (player == null) return;
        IKvStore slot = _store.Scope(IdentityOf(player));
        StateBag bag = player.State;
        foreach (string key in PersistedKeys)
            if (slot.Has(key)) bag.Set(key, slot.Get(key));
    }

    // Wire automatic save/load for a role. Only the server persists. Idempotent:
    // re-binding drops the old subscriptions first. Store/keys/resolver are left alone
    // here (Reset clears those) so a mod can configure persistence before binding.
    public static void Bind(NetRole role)
    {
        Unsubscribe();
        if (role != NetRole.Server) return;
        Subscriptions.Add(EventBus.Subscribe<ClientLeft>(e => Save(Players.Get(e.Peer)), SavePriority));
        Subscriptions.Add(EventBus.Subscribe<ClientJoined>(e => Load(Players.Get(e.Peer))));
    }

    // Full teardown for a session reload: drop subscriptions and restore defaults.
    public static void Reset()
    {
        Unsubscribe();
        _store = new MemoryKvStore();
        PersistedKeys.Clear();
        _identity = DefaultIdentity;
    }

    private static void Unsubscribe()
    {
        foreach (IDisposable sub in Subscriptions) sub.Dispose();
        Subscriptions.Clear();
    }
}
