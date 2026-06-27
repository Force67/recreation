using System;
using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Net;

// The front-end state behind the "find and join a server" screen. A static state
// machine the UI binds to: the queried server list, the active filter and sort, the
// player's favourites and history, and the connect intent the runtime acts on.
//
// A pure data model. SetServers feeds it the master-server query result; the actual
// socket connect happens engine-side. Connect surfaces the intent to the engine and
// exposes it as PendingConnect/ConnectRequested so the runtime can poll or subscribe.
//
// Favourites and history are in-memory session state, not persisted across sessions.
public static class ServerBrowser
{
    private const int HistoryCap = 16;  // recent servers kept; older visits fall off

    private static readonly ServerList _servers = new();
    private static ServerFilter _filter = new();
    private static ServerSort _sort = ServerSort.Ping;

    // Favourite addresses in insertion order (a List since order is shown and the
    // set is tiny). History is most-recent-first.
    private static readonly List<string> _favourites = new();
    private static readonly List<string> _history = new();

    // Raised whenever the favourites set changes, so a bound list re-renders.
    public static event Action? FavouritesChanged;

    // Raised when the player asks to join a server, carrying its address. Parallel
    // to PendingConnect: subscribe for a push, or poll PendingConnect for a pull.
    public static event Action<string>? ConnectRequested;

    // The address the player wants to connect to, set by Connect and cleared by
    // ConsumeConnect. The runtime polls this each frame and performs the dial.
    public static string? PendingConnect { get; private set; }

    public static ServerList Servers => _servers;

    public static ServerFilter Filter
    {
        get => _filter;
        set => _filter = value ?? new ServerFilter();
    }

    public static ServerSort Sort
    {
        get => _sort;
        set => _sort = value;
    }

    public static IReadOnlyList<string> Favourites => _favourites;
    public static IReadOnlyList<string> History => _history;

    // Replace the known servers (the master-server query result). Filter, sort,
    // favourites and history are untouched; only the underlying list changes.
    public static void SetServers(IEnumerable<ServerEntry> servers)
    {
        ArgumentNullException.ThrowIfNull(servers);
        _servers.Clear();
        _servers.AddRange(servers);
    }

    // The list the UI renders: the known servers narrowed by the active filter, then
    // ordered by the active sort. Recomputed on read so it reflects the current state.
    public static IReadOnlyList<ServerEntry> Visible =>
        ServerSortOrder.Apply(_servers.Filter(_filter), _sort);

    public static bool IsFavourite(string address) => _favourites.Contains(address);

    public static void Favourite(string address)
    {
        if (string.IsNullOrEmpty(address) || _favourites.Contains(address)) return;
        _favourites.Add(address);
        FavouritesChanged?.Invoke();
    }

    public static void Unfavourite(string address)
    {
        if (_favourites.Remove(address)) FavouritesChanged?.Invoke();
    }

    // Push an address onto the recent-history list: most-recent-first, de-duped
    // (re-visiting moves it to the front), capped at HistoryCap.
    public static void RecordVisit(string address)
    {
        if (string.IsNullOrEmpty(address)) return;
        _history.Remove(address);
        _history.Insert(0, address);
        if (_history.Count > HistoryCap)
            _history.RemoveRange(HistoryCap, _history.Count - HistoryCap);
    }

    // The single join path, used by both the list and direct-connect-by-address.
    public static void Connect(string address)
    {
        if (string.IsNullOrEmpty(address)) return;
        PendingConnect = address;
        RecordVisit(address);
        // Tell the engine to dial. Unhandled natives return Value.None, so this is safe.
        Native.CallGlobal("Net", "Connect", new[] { Value.String(address) });
        ConnectRequested?.Invoke(address);
    }

    // Take and clear the pending connect. The runtime calls this each frame; it
    // returns null when there is nothing to act on.
    public static string? ConsumeConnect()
    {
        string? pending = PendingConnect;
        PendingConnect = null;
        return pending;
    }

    // Wire the browser for a role. Nothing role-specific to wire; it takes the role
    // only for the platform's uniform Bind(role) seam. Idempotent.
    internal static void Bind(NetRole role)
    {
        Reset();
        _ = role;
    }

    // Tear the browser down, the reverse of Bind. Clears all state and detaches event
    // handlers so a reload leaks no subscriptions.
    public static void Reset()
    {
        _servers.Clear();
        _filter = new ServerFilter();
        _sort = ServerSort.Ping;
        _favourites.Clear();
        _history.Clear();
        PendingConnect = null;
        FavouritesChanged = null;
        ConnectRequested = null;
    }
}
