using System;
using System.Collections.Generic;
using System.Linq;

namespace Recreation.Net;

// How the browser orders the visible list.
public enum ServerSort
{
    Ping,         // ascending: best connection first
    PlayersDesc,  // fullest first
    PlayersAsc,   // emptiest first
    NameAsc,      // alphabetical
}

// Turns a ServerSort into an ordering.
public static class ServerSortOrder
{
    public static Comparison<ServerEntry> Comparison(ServerSort sort) => sort switch
    {
        ServerSort.Ping => (a, b) => a.Ping.CompareTo(b.Ping),
        ServerSort.PlayersDesc => (a, b) => b.Players.CompareTo(a.Players),
        ServerSort.PlayersAsc => (a, b) => a.Players.CompareTo(b.Players),
        ServerSort.NameAsc => (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase),
        _ => (_, _) => 0,
    };

    // Order a sequence by the chosen sort. OrderBy is stable, so servers that tie on
    // the key keep their input order.
    public static IReadOnlyList<ServerEntry> Apply(IEnumerable<ServerEntry> servers, ServerSort sort) =>
        servers.OrderBy(s => s, Comparer<ServerEntry>.Create(Comparison(sort))).ToList();
}
