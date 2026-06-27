using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the server-browser data model end to end: list filtering and sorting, favourites, recent history, and the connect intent.
public static class ServerBrowserTests
{
    public static void Run(Check check)
    {
        Native.Backend = new FakeBackend();
        ServerBrowser.Bind(NetRole.Standalone);

        var alpha = new ServerEntry
        {
            Address = "1.1.1.1:1", Name = "Alpha Roleplay", Gametype = "rp",
            Players = 10, MaxPlayers = 32, Ping = 40, Tags = new[] { "rp", "vanilla" },
        };
        var bravo = new ServerEntry
        {
            Address = "2.2.2.2:2", Name = "Bravo Deathmatch", Gametype = "dm",
            Players = 32, MaxPlayers = 32, Ping = 20, Tags = new[] { "pvp" }, Passworded = true,
        };
        var charlie = new ServerEntry
        {
            Address = "3.3.3.3:3", Name = "Charlie Empty", Gametype = "rp",
            Players = 0, MaxPlayers = 16, Ping = 80, Tags = new[] { "rp" },
        };
        var delta = new ServerEntry
        {
            Address = "4.4.4.4:4", Name = "Delta RP", Gametype = "rp",
            Players = 5, MaxPlayers = 16, Ping = 10, Tags = Array.Empty<string>(),
        };

        check.That("IsFull reflects a full server", bravo.IsFull);
        check.That("IsFull false for a server with room", !alpha.IsFull);

        var list = new ServerList();
        list.AddRange(new[] { alpha, bravo, charlie, delta });
        check.Equal("list holds every server", 4, list.Count);

        // --- Filter: name substring is case-insensitive ---
        var byName = list.Filter(new ServerFilter { Name = "ALPHA" }).ToList();
        check.Equal("name filter is a case-insensitive substring", 1, byName.Count);
        check.Equal("name filter found the right server", alpha, byName[0]);

        // --- Filter: hideFull drops the only full server ---
        var notFull = list.Filter(new ServerFilter { HideFull = true }).ToList();
        check.That("hideFull drops the full server", !notFull.Contains(bravo));
        check.Equal("hideFull keeps the rest", 3, notFull.Count);

        // --- Filter: hideEmpty drops the empty one ---
        var notEmpty = list.Filter(new ServerFilter { HideEmpty = true }).ToList();
        check.That("hideEmpty drops the empty server", !notEmpty.Contains(charlie));
        check.Equal("hideEmpty keeps the rest", 3, notEmpty.Count);

        // --- Filter: hidePassworded drops the locked one ---
        var noPass = list.Filter(new ServerFilter { HidePassworded = true }).ToList();
        check.That("hidePassworded drops the locked server", !noPass.Contains(bravo));
        check.Equal("hidePassworded keeps the rest", 3, noPass.Count);

        // --- Filter: a required tag keeps only tagged servers ---
        var rpTag = list.Filter(new ServerFilter { RequiredTag = "RP" }).ToList();
        check.Equal("required tag yields the tagged subset", 2, rpTag.Count);
        check.That("required tag keeps both rp-tagged servers",
            rpTag.Contains(alpha) && rpTag.Contains(charlie));
        check.That("required tag drops the untagged server", !rpTag.Contains(delta));

        // --- Filter: criteria combine (AND) ---
        var rpOpen = list.Filter(new ServerFilter { Gametype = "rp", HideEmpty = true }).ToList();
        check.That("combined filter keeps rp + populated", rpOpen.Contains(alpha) && rpOpen.Contains(delta));
        check.That("combined filter drops the empty rp server", !rpOpen.Contains(charlie));
        check.Equal("combined filter subset size", 2, rpOpen.Count);

        // --- Sort: Ping ascending (delta 10, bravo 20, alpha 40, charlie 80) ---
        var byPing = list.Sort(ServerSort.Ping);
        check.That("ping sort is ascending",
            byPing.SequenceEqual(new[] { delta, bravo, alpha, charlie }));

        // --- Sort: PlayersDesc (bravo 32, alpha 10, delta 5, charlie 0) ---
        var byPlayers = list.Sort(ServerSort.PlayersDesc);
        check.That("players-desc puts the fullest first",
            byPlayers.SequenceEqual(new[] { bravo, alpha, delta, charlie }));

        // --- SetServers + Visible reflects the active filter and sort ---
        ServerBrowser.SetServers(new[] { alpha, bravo, charlie, delta });
        ServerBrowser.Filter = new ServerFilter { Gametype = "rp" };
        ServerBrowser.Sort = ServerSort.Ping;
        var visible = ServerBrowser.Visible;
        // rp only -> alpha, charlie, delta; by ping -> delta(10), alpha(40), charlie(80)
        check.That("Visible applies the filter then the sort",
            visible.SequenceEqual(new[] { delta, alpha, charlie }));
        check.That("Visible excludes the filtered-out server", !visible.Contains(bravo));

        // --- Favourites: add/remove, ordering, IsFavourite, FavouritesChanged ---
        int favEvents = 0;
        ServerBrowser.FavouritesChanged += () => favEvents++;
        check.That("nothing favourited initially", !ServerBrowser.IsFavourite(alpha.Address));
        ServerBrowser.Favourite(alpha.Address);
        ServerBrowser.Favourite(bravo.Address);
        check.That("favourite is recorded", ServerBrowser.IsFavourite(alpha.Address));
        check.That("favourites keep insertion order",
            ServerBrowser.Favourites.SequenceEqual(new[] { alpha.Address, bravo.Address }));
        check.Equal("FavouritesChanged fires once per add", 2, favEvents);
        ServerBrowser.Favourite(alpha.Address);  // duplicate is a no-op
        check.Equal("favourites are an ordered set (duplicate ignored, no event)", 2, favEvents);
        check.Equal("favourites count unchanged after duplicate", 2, ServerBrowser.Favourites.Count);
        ServerBrowser.Unfavourite(alpha.Address);
        check.That("unfavourite removes it", !ServerBrowser.IsFavourite(alpha.Address));
        check.Equal("unfavourite fires a change", 3, favEvents);

        // --- History: most-recent-first, de-duped, capped ---
        ServerBrowser.RecordVisit("a");
        ServerBrowser.RecordVisit("b");
        ServerBrowser.RecordVisit("a");  // re-visit moves to the front
        check.That("history is most-recent-first and de-duped",
            ServerBrowser.History.SequenceEqual(new[] { "a", "b" }));
        for (int i = 0; i < 20; i++) ServerBrowser.RecordVisit($"h{i}");
        check.Equal("history is capped", 16, ServerBrowser.History.Count);
        check.Equal("most recent visit is at the front", "h19", ServerBrowser.History[0]);

        // --- Connect: sets the intent, records a visit, raises the event, calls native ---
        int connectEvents = 0;
        string? connectedTo = null;
        ServerBrowser.ConnectRequested += addr => { connectEvents++; connectedTo = addr; };
        ServerBrowser.Connect("9.9.9.9:9");
        check.Equal("connect sets the pending intent", "9.9.9.9:9", ServerBrowser.PendingConnect);
        check.Equal("connect raises ConnectRequested once", 1, connectEvents);
        check.Equal("ConnectRequested carries the address", "9.9.9.9:9", connectedTo);
        check.Equal("connect records the visit at the front", "9.9.9.9:9", ServerBrowser.History[0]);

        var fake = (FakeBackend)Native.Backend!;
        check.Equal("connect surfaces the intent to the engine", "Net", fake.LastGlobalType);
        check.Equal("connect calls Net.Connect", "Connect", fake.LastGlobalFunction);
        check.Equal("connect passes the address to the native", "9.9.9.9:9", fake.LastStringArg);

        string? consumed = ServerBrowser.ConsumeConnect();
        check.Equal("consume returns the pending address", "9.9.9.9:9", consumed);
        check.That("consume clears the pending intent", ServerBrowser.PendingConnect == null);
        check.That("consume again yields nothing", ServerBrowser.ConsumeConnect() == null);

        // --- Reset clears all browser state ---
        ServerBrowser.Favourite("keep:1");
        ServerBrowser.Connect("pending:1");
        ServerBrowser.Reset();
        check.Equal("reset clears the server list", 0, ServerBrowser.Servers.Count);
        check.Equal("reset clears favourites", 0, ServerBrowser.Favourites.Count);
        check.Equal("reset clears history", 0, ServerBrowser.History.Count);
        check.That("reset clears the pending connect", ServerBrowser.PendingConnect == null);
        // The active filter is back to its default (matches everything).
        ServerBrowser.SetServers(new[] { alpha, bravo });
        check.Equal("reset restored the default match-all filter", 2, ServerBrowser.Visible.Count);

        ServerBrowser.Reset();
        EventBus.Clear();
        Native.Backend = null;
    }
}
