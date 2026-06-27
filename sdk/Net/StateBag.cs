using System;
using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Net;

// One networked change to a state bag. FromServer is true when it came from the
// host, false when it originated locally.
public readonly struct StateBagChange
{
    public readonly StateBag Bag;
    public readonly string Key;
    public readonly Value Value;     // the new value (None means the key was removed)
    public readonly Value Previous;  // what it held before
    public readonly bool FromServer;

    public StateBagChange(StateBag bag, string key, Value value, Value previous, bool fromServer)
    {
        Bag = bag;
        Key = key;
        Value = value;
        Previous = previous;
        FromServer = fromServer;
    }

    public bool Removed => Value.IsNone;
}

// A named, replicated key/value store, server-authoritative by default. A bag is a
// thin data holder; replication policy lives in StateBags, so the bag has no notion
// of the network.
public sealed class StateBag
{
    private readonly Dictionary<string, Value> _entries = new();
    private readonly Dictionary<string, List<Action<StateBagChange>>> _keyHandlers = new();
    private readonly List<Action<StateBagChange>> _anyHandlers = new();

    public string Name { get; }

    internal StateBag(string name) => Name = name;

    // The current value for a key, or None when unset (None and absent are the same).
    public Value Get(string key) => _entries.TryGetValue(key, out Value v) ? v : Value.None;

    public bool Has(string key) => _entries.ContainsKey(key);

    public int Count => _entries.Count;

    public IReadOnlyCollection<string> Keys => _entries.Keys;

    // Snapshot of the entries (a copy, so iteration is safe while handlers mutate).
    internal IReadOnlyList<KeyValuePair<string, Value>> Snapshot()
    {
        var list = new List<KeyValuePair<string, Value>>(_entries.Count);
        foreach (KeyValuePair<string, Value> kv in _entries) list.Add(kv);
        return list;
    }

    // Set (or, with a None value, clear) a key. `replicated` false keeps the change
    // local; true routes it through the manager's authority and replication rules.
    public void Set(string key, Value value, bool replicated = true) =>
        StateBags.SetFrom(this, key, value, replicated);

    // Convenience overloads for the common scalar shapes.
    public void Set(string key, int value, bool replicated = true) => Set(key, Value.Int(value), replicated);
    public void Set(string key, float value, bool replicated = true) => Set(key, Value.Float(value), replicated);
    public void Set(string key, bool value, bool replicated = true) => Set(key, Value.Bool(value), replicated);
    public void Set(string key, string value, bool replicated = true) => Set(key, Value.String(value), replicated);

    public void Remove(string key, bool replicated = true) => Set(key, Value.None, replicated);

    // Subscribe to changes for one key. The returned token detaches the handler.
    public IDisposable OnChange(string key, Action<StateBagChange> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        if (!_keyHandlers.TryGetValue(key, out List<Action<StateBagChange>>? list))
        {
            list = new List<Action<StateBagChange>>();
            _keyHandlers[key] = list;
        }
        list.Add(handler);
        return new Unsubscriber(() => list.Remove(handler));
    }

    // Subscribe to changes for every key in this bag.
    public IDisposable OnChange(Action<StateBagChange> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        _anyHandlers.Add(handler);
        return new Unsubscriber(() => _anyHandlers.Remove(handler));
    }

    // Apply a mutation to the local store, firing subscribers only on a real change.
    // Returns true on a real change so the manager only puts genuine deltas on the wire.
    internal bool ApplyLocal(string key, Value value, bool fromServer)
    {
        Value previous = Get(key);
        if (ValuesEqual(previous, value)) return false;

        if (value.IsNone) _entries.Remove(key);
        else _entries[key] = value;

        var change = new StateBagChange(this, key, value, previous, fromServer);
        Fire(_anyHandlers, change);
        if (_keyHandlers.TryGetValue(key, out List<Action<StateBagChange>>? list)) Fire(list, change);
        StateBags.RaiseGlobalChange(change);  // manager-level observers (player roster, ...)
        return true;
    }

    // Snapshot the handler list so a handler may (un)subscribe mid-dispatch, and
    // isolate a thrower so the rest still run.
    private static void Fire(List<Action<StateBagChange>> handlers, in StateBagChange change)
    {
        if (handlers.Count == 0) return;
        Action<StateBagChange>[] snapshot = handlers.ToArray();
        foreach (Action<StateBagChange> h in snapshot)
        {
            try { h(change); }
            catch (Exception ex) { Console.Error.WriteLine($"[statebag] handler threw: {ex.Message}"); }
        }
    }

    // Value carries no equality of its own; compare by kind and payload.
    internal static bool ValuesEqual(Value a, Value b)
    {
        if (a.Kind != b.Kind) return false;
        return a.Kind switch
        {
            ValueKind.None => true,
            ValueKind.Float => a.AsFloat() == b.AsFloat(),
            ValueKind.String => a.AsString() == b.AsString(),
            _ => a.AsHandle() == b.AsHandle(),  // Int/Bool/Object/Array share the scalar slot
        };
    }
}
