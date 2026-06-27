using System;
using System.Linq;

namespace Recreation.Net;

// The criteria the browser narrows the server list by. Every field is optional: an
// unset field is not tested, so a fresh filter matches everything. A server passes
// only when it satisfies every set criterion (AND).
public sealed class ServerFilter
{
    // Case-insensitive substring of the server name; null/empty means "any name".
    public string? Name { get; set; }

    // Exact gametype (case-insensitive); null/empty means "any gametype".
    public string? Gametype { get; set; }

    public bool HideFull { get; set; }
    public bool HideEmpty { get; set; }
    public bool HidePassworded { get; set; }

    // A tag the server must carry (case-insensitive); null/empty means "any".
    public string? RequiredTag { get; set; }

    public bool Matches(ServerEntry server)
    {
        ArgumentNullException.ThrowIfNull(server);

        if (!string.IsNullOrEmpty(Name) &&
            !server.Name.Contains(Name, StringComparison.OrdinalIgnoreCase))
            return false;

        if (!string.IsNullOrEmpty(Gametype) &&
            !string.Equals(server.Gametype, Gametype, StringComparison.OrdinalIgnoreCase))
            return false;

        if (HideFull && server.IsFull) return false;
        if (HideEmpty && server.Players == 0) return false;
        if (HidePassworded && server.Passworded) return false;

        if (!string.IsNullOrEmpty(RequiredTag) &&
            !server.Tags.Contains(RequiredTag, StringComparer.OrdinalIgnoreCase))
            return false;

        return true;
    }
}
