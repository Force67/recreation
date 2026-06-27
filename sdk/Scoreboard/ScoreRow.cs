using System.Collections.Generic;

namespace Recreation.Net;

// One built scoreboard line: the player, the rendered cell strings (index-aligned
// to Scoreboard.Columns), and the integer sort key. An immutable snapshot.
public sealed class ScoreRow
{
    public Player Player { get; }
    public IReadOnlyList<string> Cells { get; }

    // The value the row was sorted on (the sort key extractor applied to Player).
    public int SortKey { get; }

    public ScoreRow(Player player, IReadOnlyList<string> cells, int sortKey)
    {
        Player = player;
        Cells = cells;
        SortKey = sortKey;
    }
}
