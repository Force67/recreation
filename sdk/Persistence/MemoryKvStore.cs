using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Net;

// The default in-memory store: a plain dictionary, no durability. Flush is a no-op.
public sealed class MemoryKvStore : IKvStore
{
    private readonly Dictionary<string, Value> _entries = new();

    public Value Get(string key) => _entries.TryGetValue(key, out Value v) ? v : Value.None;

    public void Set(string key, Value value)
    {
        // None means "absent"; storing it would leave a phantom key, so delete.
        if (value.IsNone) _entries.Remove(key);
        else _entries[key] = value;
    }

    public bool Has(string key) => _entries.ContainsKey(key);

    public void Delete(string key) => _entries.Remove(key);

    public IReadOnlyCollection<string> Keys => _entries.Keys;

    public void Flush() { }
}
