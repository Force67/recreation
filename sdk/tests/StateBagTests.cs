using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the networked state-bag primitive across all three roles: standalone is local,
// the server applies/broadcasts/validates, and a client's requests land only on the server's echo.
public static class StateBagTests
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
        // --- standalone: a set applies locally and fires handlers, no network ---
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Standalone);

        int fired = 0;
        Value seen = Value.None;
        bool sawFromServer = true;
        IDisposable sub = StateBags.Global.OnChange("score", c =>
        {
            fired++;
            seen = c.Value;
            sawFromServer = c.FromServer;
        });
        StateBags.Global.Set("score", 10);
        check.Equal("standalone set fires handler", 1, fired);
        check.Equal("standalone value applied", 10, StateBags.Global.Get("score").AsInt());
        check.That("standalone change is local, not from server", !sawFromServer);
        check.Equal("standalone never hits the network", 0, rec.Emits.Count);

        StateBags.Global.Set("score", 10);
        check.Equal("a no-op set does not refire", 1, fired);

        StateBags.Global.Remove("score");
        check.That("removed key reports None", seen.IsNone);
        check.That("removed key is gone", !StateBags.Global.Has("score"));
        sub.Dispose();
        int firedAtDispose = fired;
        StateBags.Global.Set("score", 1);
        check.Equal("disposed handler stops firing", firedAtDispose, fired);

        // --- server: a set applies locally and broadcasts to every client ---
        Rpc.Clear();
        rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Server);
        StateBags.Global.Set("weather", "rain");
        check.Equal("server applies locally", "rain", StateBags.Global.Get("weather").AsString());
        check.Equal("server broadcasts the change", 1, rec.Emits.Count);
        check.Equal("broadcast target", RpcTarget.Broadcast, rec.Emits[^1].Target);
        check.Equal("broadcast name", "sb:set", rec.Emits[^1].Name);
        check.Equal("broadcast bag", "global", rec.Emits[^1].Args[0].AsString());
        check.Equal("broadcast key", "weather", rec.Emits[^1].Args[1].AsString());
        check.Equal("broadcast value", "rain", rec.Emits[^1].Args[2].AsString());

        // --- server: a joining player is sent the whole current state ---
        rec.Emits.Clear();
        EventBus.Publish(new ClientJoined(7u));
        check.That("join sync sends state to the new peer", rec.Emits.Count >= 1);
        check.Equal("sync is targeted at that peer", RpcTarget.ToClient, rec.Emits[0].Target);
        check.Equal("sync peer", 7u, rec.Emits[0].Peer);
        check.Equal("sync carries the value", "rain", rec.Emits[0].Args[2].AsString());

        // --- server: a client may write its own player bag ---
        rec.Emits.Clear();
        Rpc.Dispatch("sb:req", 7u, false,
            new[] { Value.String("player:7"), Value.String("name"), Value.String("Bob") });
        check.Equal("own-bag client write applied", "Bob", StateBags.Player(7).Get("name").AsString());
        check.Equal("accepted write is rebroadcast", "sb:set", rec.Emits[^1].Name);

        // --- server: a client may not write another player's bag (default policy) ---
        rec.Emits.Clear();
        Rpc.Dispatch("sb:req", 7u, false,
            new[] { Value.String("player:8"), Value.String("name"), Value.String("Eve") });
        check.That("cross-player client write rejected", !StateBags.Player(8).Has("name"));
        check.Equal("rejected write is not rebroadcast", 0, rec.Emits.Count);

        // --- server: a custom validator can widen the policy ---
        StateBags.SetClientWriteValidator((peer, bag, key, val) => true);
        Rpc.Dispatch("sb:req", 7u, false,
            new[] { Value.String("global"), Value.String("open"), Value.Bool(true) });
        check.That("custom validator allows the write", StateBags.Global.Get("open").AsBool());

        // --- server: a leaving player's bag is dropped and clients are told ---
        StateBags.Player(9).Set("name", "Zed");
        rec.Emits.Clear();
        EventBus.Publish(new ClientLeft(9u));
        check.That("left player's bag is forgotten", StateBags.Find("player:9") == null);
        check.That("clients told to drop the bag",
            rec.Emits.Exists(e => e.Name == "sb:drop" && e.Args[0].AsString() == "player:9"));

        // --- client: a set is a request and does not apply until the server echoes ---
        Rpc.Clear();
        rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Client);
        int clientFired = 0;
        bool clientFromServer = false;
        StateBags.Player(3).OnChange("hp", c =>
        {
            clientFired++;
            clientFromServer = c.FromServer;
        });
        StateBags.Player(3).Set("hp", 100);
        check.That("client set does not apply optimistically", !StateBags.Player(3).Has("hp"));
        check.Equal("client set is a request", "sb:req", rec.Emits[^1].Name);
        check.Equal("client request goes to the server", RpcTarget.ToServer, rec.Emits[^1].Target);

        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("player:3"), Value.String("hp"), Value.Int(100) });
        check.Equal("client applies the authoritative echo", 100, StateBags.Player(3).Get("hp").AsInt());
        check.Equal("client handler fired once", 1, clientFired);
        check.That("client change is marked from server", clientFromServer);

        // a local-only set still applies on a client (UI state that never replicates)
        StateBags.Player(3).Set("ui_open", true, replicated: false);
        check.That("local-only client set applies", StateBags.Player(3).Get("ui_open").AsBool());

        Rpc.Dispatch("sb:drop", 0u, true, new[] { Value.String("player:3") });
        check.That("client drops the bag on a server drop", StateBags.Find("player:3") == null);

        Platform.Reset();
        Rpc.Clear();
    }
}
