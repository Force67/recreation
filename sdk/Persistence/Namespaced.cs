using System;
using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Net;

// A prefixed view onto another store, so callers can share one backing store without
// key collisions: every key is prefixed (player:5 -> "player:5:gold") and Keys strips
// the prefix back off. Obtained via IKvStore.Scope.
public sealed class KvNamespace : IKvStore
{
    private readonly IKvStore _inner;
    private readonly string _prefix;  // includes the trailing ':' separator

    internal KvNamespace(IKvStore inner, string prefix)
    {
        ArgumentNullException.ThrowIfNull(inner);
        _inner = inner;
        _prefix = prefix + ":";
    }

    private string Full(string key) => _prefix + key;

    public Value Get(string key) => _inner.Get(Full(key));

    public void Set(string key, Value value) => _inner.Set(Full(key), value);

    public bool Has(string key) => _inner.Has(Full(key));

    public void Delete(string key) => _inner.Delete(Full(key));

    // Flush hits the real backing store.
    public void Flush() => _inner.Flush();

    public IReadOnlyCollection<string> Keys
    {
        get
        {
            var keys = new List<string>();
            foreach (string k in _inner.Keys)
                if (k.StartsWith(_prefix, StringComparison.Ordinal))
                    keys.Add(k.Substring(_prefix.Length));
            return keys;
        }
    }
}
