using System;

namespace Recreation.Net;

// A column in the scoreboard: a header label and how to render one player's cell.
// A mod pairs a label with an extractor that reads the player's replicated state:
//   new ScoreColumn("Score", p => p.State.Get("score").AsInt().ToString())
public sealed class ScoreColumn
{
    public string Header { get; }

    // Renders this column's text for one player.
    public Func<Player, string> Cell { get; }

    public ScoreColumn(string header, Func<Player, string> cell)
    {
        ArgumentNullException.ThrowIfNull(header);
        ArgumentNullException.ThrowIfNull(cell);
        Header = header;
        Cell = cell;
    }

    // The roster-resolved display name (falls back to "Player N" until one is set).
    public static ScoreColumn Name { get; } = new("Name", p => p.Name);

    // Score and ping read the conventional stat keys written into each player's
    // state bag; an unset key reads as 0.
    public static ScoreColumn Score { get; } = new("Score", p => p.State.Get("score").AsInt().ToString());
    public static ScoreColumn Ping { get; } = new("Ping", p => p.State.Get("ping").AsInt().ToString());
}
