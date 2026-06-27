using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Net;

// Durable key/value contract for server-side storage that must outlive a session
// (player saves, world progress, mod data). Not networked: never replicates, host
// writes only.
public interface IKvStore
{
    // The stored value for a key, or Value.None when unset (None and absent are the same).
    Value Get(string key);

    // Store a value under a key. Setting Value.None deletes the key.
    void Set(string key, Value value);

    bool Has(string key);

    void Delete(string key);

    IReadOnlyCollection<string> Keys { get; }

    // Persist any buffered writes now. A no-op for stores that write eagerly.
    void Flush();
}

// Scope on any IKvStore (including a concrete type, where a default interface method
// would not surface). Returns a view that prefixes every key.
public static class KvStoreExtensions
{
    public static IKvStore Scope(this IKvStore store, string prefix) => new KvNamespace(store, prefix);
}
