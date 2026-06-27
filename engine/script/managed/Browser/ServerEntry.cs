using System;

namespace Recreation.Net;

// One server as the browser knows it: the address to dial and the metadata a
// master-server query reports. Immutable: a refresh replaces entries rather than
// mutating them.
public sealed record ServerEntry
{
    // Where to connect, "host:port". Also the stable identity used by favourites and
    // history.
    public required string Address { get; init; }

    public string Name { get; init; } = "";
    public string Gametype { get; init; } = "";
    public int Players { get; init; }
    public int MaxPlayers { get; init; }
    public int Ping { get; init; }                       // round-trip latency, ms
    public string[] Tags { get; init; } = Array.Empty<string>();
    public bool Passworded { get; init; }

    // A server with no free slots. Drives the join-disabled state and the hide-full
    // filter.
    public bool IsFull => Players >= MaxPlayers;
}
