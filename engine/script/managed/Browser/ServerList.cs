using System;
using System.Collections.Generic;

namespace Recreation.Net;

// A mutable bag of servers with the two queries the browser needs: narrow by a
// filter, order by a sort. The filter/sort logic lives in ServerFilter and
// ServerSortOrder.
public sealed class ServerList
{
    private readonly List<ServerEntry> _entries = new();

    public void Add(ServerEntry server)
    {
        ArgumentNullException.ThrowIfNull(server);
        _entries.Add(server);
    }

    public void AddRange(IEnumerable<ServerEntry> servers)
    {
        ArgumentNullException.ThrowIfNull(servers);
        _entries.AddRange(servers);
    }

    public void Clear() => _entries.Clear();

    public IReadOnlyList<ServerEntry> All => _entries;

    public int Count => _entries.Count;

    // Lazily yield the servers passing the filter, in list order, so a caller can
    // stop early or chain further LINQ.
    public IEnumerable<ServerEntry> Filter(ServerFilter filter)
    {
        ArgumentNullException.ThrowIfNull(filter);
        foreach (ServerEntry server in _entries)
            if (filter.Matches(server))
                yield return server;
    }

    // A new ordered snapshot; the list itself keeps its insertion order.
    public IReadOnlyList<ServerEntry> Sort(ServerSort sort) => ServerSortOrder.Apply(_entries, sort);
}
