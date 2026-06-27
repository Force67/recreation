using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the Tab player list as a pure view over replicated player state: Build's snapshot, the default sort, and the open/close lifecycle.
public static class ScoreboardTests
{
    private sealed class Recording : IRpcBackend
    {
        public readonly List<(RpcTarget Target, uint Peer, string Name, Value[] Args)> Emits = new();
        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args) =>
            Emits.Add((target, peer, name, args.ToArray()));
        public void Subscribe(string name) { }
    }

    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        Rpc.Clear();
        Rpc.Bind(new Recording());

        Platform.Boot(NetRole.Server);
        Scoreboard.Bind(NetRole.Server);

        check.Equal("seeds the three default columns", 3, Scoreboard.Columns.Count);
        check.That("starts closed", !Scoreboard.IsOpen);

        // Three players with names and scores; two tie on score to test the tie-break.
        foreach (uint id in new uint[] { 1, 2, 3 }) EventBus.Publish(new ClientJoined(id));
        StateBags.Player(1u).Set("name", "Bram");
        StateBags.Player(1u).Set("score", 30);
        StateBags.Player(2u).Set("name", "Aela");
        StateBags.Player(2u).Set("score", 50);
        StateBags.Player(3u).Set("name", "Cael");
        StateBags.Player(3u).Set("score", 50);

        // --- Build returns one row per present player, with the right cells ---
        IReadOnlyList<ScoreRow> rows = Scoreboard.Build();
        check.Equal("one row per present player", 3, rows.Count);
        check.Equal("name cell is the player's name", "Aela", rows[0].Cells[0]);
        check.Equal("score cell renders the score stat", "50", rows[0].Cells[1]);

        // --- Sorting: score descending, ties broken by name ascending ---
        check.Equal("highest score first", "Aela", rows[0].Player.Name);
        check.Equal("tie broken by name", "Cael", rows[1].Player.Name);
        check.Equal("lowest score last", "Bram", rows[2].Player.Name);

        // --- A removed player changes the built rows ---
        EventBus.Publish(new ClientLeft(2u));  // server drops player:2's bag
        rows = Scoreboard.Build();
        check.Equal("removing a player drops its row", 2, rows.Count);
        check.That("removed player is gone", rows.All(r => r.Player.Id != 2u));

        // --- A re-added player reappears ---
        EventBus.Publish(new ClientJoined(2u));
        StateBags.Player(2u).Set("name", "Aela");
        StateBags.Player(2u).Set("score", 50);
        check.Equal("re-adding a player restores its row", 3, Scoreboard.Build().Count);

        // --- Open / Close / Toggle flip IsOpen; Build works regardless ---
        Scoreboard.Open();
        check.That("Open sets IsOpen", Scoreboard.IsOpen);
        check.Equal("Build works while open", 3, Scoreboard.Build().Count);
        Scoreboard.Toggle();
        check.That("Toggle closes an open board", !Scoreboard.IsOpen);
        Scoreboard.Toggle();
        check.That("Toggle opens a closed board", Scoreboard.IsOpen);
        Scoreboard.Close();
        check.That("Close clears IsOpen", !Scoreboard.IsOpen);
        check.Equal("Close hides the HUD board", "HideScoreboard", fake.LastGlobalFunction);
        check.Equal("Build works while closed", 3, Scoreboard.Build().Count);

        // --- A custom column extractor appears in the row cells ---
        Scoreboard.AddColumn(new ScoreColumn("Kills", p => p.State.Get("kills").AsInt().ToString()));
        StateBags.Player(1u).Set("kills", 7);
        ScoreRow bram = Scoreboard.Build().First(r => r.Player.Id == 1u);
        check.Equal("custom column renders its cell", "7", bram.Cells[^1]);

        // --- A custom sort key reorders the board (ascending by score) ---
        Scoreboard.SortBy(p => p.State.Get("score").AsInt(), descending: false);
        check.Equal("ascending sort puts the lowest score first", "Bram", Scoreboard.Build()[0].Player.Name);

        // --- Reset clears columns and state back to the empty baseline ---
        Scoreboard.Reset();
        check.Equal("Reset clears the columns", 0, Scoreboard.Columns.Count);
        check.That("Reset closes the board", !Scoreboard.IsOpen);

        Recreation.Net.Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
