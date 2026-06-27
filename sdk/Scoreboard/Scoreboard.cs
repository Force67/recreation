using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The Tab player list: rows of connected players a user holds Tab to see. A pure
// view over replicated player state. Columns read each player's state bag, the
// board sorts the rows, and rendering pushes them to the engine HUD.
//
// HUD encoding (a header call, then one call per row, then a hide on close):
//   Hud.Scoreboard(title, columnCount, header0, header1, ...)  - begins a frame
//   Hud.ScoreboardRow(playerId, cell0, cell1, ...)             - one per sorted row
//   Hud.HideScoreboard()                                       - on close
public static class Scoreboard
{
    private const string DefaultTitle = "Players";

    private static readonly List<ScoreColumn> _columns = new();
    private static readonly List<IDisposable> Subscriptions = new();

    private static Func<Player, int> _sortKey = DefaultSortKey;
    private static bool _descending = true;
    private static NetRole _role = NetRole.Standalone;
    private static string _title = DefaultTitle;

    public static NetRole Role => _role;

    // True while the board is held open.
    public static bool IsOpen { get; private set; }

    // The active columns in render order.
    public static IReadOnlyList<ScoreColumn> Columns => _columns;

    // The server name / gametype line shown atop the board. Re-renders an open board
    // when it changes.
    public static string Title
    {
        get => _title;
        set { _title = value ?? string.Empty; RenderIfOpen(); }
    }

    // Wire the board for a role: reset, seed the default columns (Name, Score, Ping)
    // and the default sort, then watch the roster and player stats so an open board
    // re-renders on change. Idempotent.
    public static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        SetColumns(ScoreColumn.Name, ScoreColumn.Score, ScoreColumn.Ping);
        Subscriptions.Add(EventBus.Subscribe<PlayerJoined>(_ => RenderIfOpen()));
        Subscriptions.Add(EventBus.Subscribe<PlayerLeft>(_ => RenderIfOpen()));
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
    }

    // Drop subscriptions and return to a neutral baseline: no columns, default sort
    // and title, closed. Bind re-seeds the default columns.
    public static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        _columns.Clear();
        _sortKey = DefaultSortKey;
        _descending = true;
        _title = DefaultTitle;
        IsOpen = false;
        _role = NetRole.Standalone;
    }

    public static void AddColumn(ScoreColumn column)
    {
        ArgumentNullException.ThrowIfNull(column);
        _columns.Add(column);
        RenderIfOpen();
    }

    public static void SetColumns(params ScoreColumn[] columns)
    {
        _columns.Clear();
        foreach (ScoreColumn c in columns)
            if (c != null) _columns.Add(c);
        RenderIfOpen();
    }

    // Sort the rows by an integer key, highest first by default. The default key is
    // each player's "score" stat, descending.
    public static void SortBy(Func<Player, int> key, bool descending = true)
    {
        ArgumentNullException.ThrowIfNull(key);
        _sortKey = key;
        _descending = descending;
        RenderIfOpen();
    }

    public static void Open()
    {
        IsOpen = true;
        Render();
    }

    public static void Close()
    {
        if (!IsOpen) return;
        IsOpen = false;
        Native.CallGlobal("Hud", "HideScoreboard", Array.Empty<Value>());
    }

    public static void Toggle()
    {
        if (IsOpen) Close();
        else Open();
    }

    // Snapshot the roster, render every column's cell for each present player, and
    // sort. Never touches the HUD, so it returns the same rows on any role.
    public static IReadOnlyList<ScoreRow> Build()
    {
        var rows = new List<ScoreRow>(Players.Count);
        foreach (Player p in Players.All)
        {
            var cells = new string[_columns.Count];
            for (int i = 0; i < _columns.Count; i++) cells[i] = _columns[i].Cell(p) ?? string.Empty;
            rows.Add(new ScoreRow(p, cells, _sortKey(p)));
        }
        rows.Sort(Compare);
        return rows;
    }

    // Sort by the key (direction per _descending), breaking ties by name for a
    // stable order.
    private static int Compare(ScoreRow a, ScoreRow b)
    {
        int cmp = _descending ? b.SortKey.CompareTo(a.SortKey) : a.SortKey.CompareTo(b.SortKey);
        return cmp != 0 ? cmp : string.CompareOrdinal(a.Player.Name, b.Player.Name);
    }

    private static void RenderIfOpen()
    {
        if (IsOpen) Render();
    }

    // Push the current board to the HUD using the documented encoding.
    private static void Render()
    {
        IReadOnlyList<ScoreRow> rows = Build();

        var header = new List<Value>(2 + _columns.Count)
        {
            Value.String(_title),
            Value.Int(_columns.Count),
        };
        foreach (ScoreColumn c in _columns) header.Add(Value.String(c.Header));
        Native.CallGlobal("Hud", "Scoreboard", header.ToArray());

        foreach (ScoreRow row in rows)
        {
            var args = new List<Value>(1 + row.Cells.Count) { Value.Int((int)row.Player.Id) };
            foreach (string cell in row.Cells) args.Add(Value.String(cell));
            Native.CallGlobal("Hud", "ScoreboardRow", args.ToArray());
        }
    }

    // Redraw an open board on player stat changes. Limited to player:* bags so
    // global/entity changes do not force a redraw.
    private static void OnBagChange(StateBagChange change)
    {
        if (change.Bag.Name.StartsWith("player:", StringComparison.Ordinal)) RenderIfOpen();
    }

    private static int DefaultSortKey(Player p) => p.State.Get("score").AsInt();
}
