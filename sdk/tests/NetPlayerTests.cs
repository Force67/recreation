using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the player registry across roles: single-player has one local player,
// the server admits peers and tells each its id, a client builds the roster from replicated presence.
public static class NetPlayerTests
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
        // --- standalone: exactly one local player ---
        Rpc.Clear();
        Rpc.Bind(new Recording());
        var joined = new List<uint>();
        var left = new List<uint>();
        EventBus.Subscribe<PlayerJoined>(e => joined.Add(e.Player.Id));
        EventBus.Subscribe<PlayerLeft>(e => left.Add(e.Player.Id));

        Platform.Boot(NetRole.Standalone);
        check.Equal("standalone has one player", 1, Players.Count);
        check.Equal("standalone local id is 0", 0u, Players.LocalId);
        check.That("standalone local player is local", Players.Local.IsLocal);
        check.That("standalone player join was raised", joined.Contains(0u));

        // --- server: a peer joins, is told its id, and enters the roster ---
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        joined.Clear();
        left.Clear();
        Platform.Boot(NetRole.Server);

        EventBus.Publish(new ClientJoined(5u));
        check.That("server registers the joined peer", Players.IsConnected(5u));
        check.That("server raises PlayerJoined for the peer", joined.Contains(5u));
        check.That("server tells the peer its id",
            rec.Emits.Exists(e => e.Name == "net:you" && e.Target == RpcTarget.ToClient &&
                                  e.Peer == 5u && e.Args[0].AsInt() == 5));
        check.That("server replicates the peer's presence",
            rec.Emits.Exists(e => e.Name == "sb:set" && e.Args[0].AsString() == "player:5" &&
                                  e.Args[1].AsString() == "present"));
        check.Equal("a fresh peer's name is a placeholder", "Player 5", Players.Get(5u)!.Name);

        // the player can be named and given an actor (server-authoritative writes)
        Players.Get(5u)!.SetName("Lydia");
        check.Equal("named player reads back", "Lydia", Players.Get(5u)!.Name);
        Players.Get(5u)!.SetActor(0x140u);
        check.Equal("player's actor reads back", 0x140u, Players.Get(5u)!.Actor);

        // --- server: the peer leaves and is removed ---
        EventBus.Publish(new ClientLeft(5u));
        check.That("server removes the left peer", !Players.IsConnected(5u));
        check.That("server raises PlayerLeft for the peer", left.Contains(5u));

        // --- client: identity and roster come entirely from the server ---
        Rpc.Clear();
        Rpc.Bind(new Recording());
        joined.Clear();
        left.Clear();
        Platform.Boot(NetRole.Client);

        Rpc.Dispatch("net:you", 0u, true, new[] { Value.Int(9) });
        check.Equal("client learns its id from the server", 9u, Players.LocalId);

        // server announces two players (the local one and a peer) via presence
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("player:9"), Value.String("present"), Value.Bool(true) });
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("player:4"), Value.String("present"), Value.Bool(true) });
        check.Equal("client roster has both players", 2, Players.Count);
        check.That("client knows the local player", Players.Local.IsLocal && Players.Local.Id == 9u);
        check.That("client sees the other player", Players.IsConnected(4u));
        check.That("client raised joins for both", joined.Contains(9u) && joined.Contains(4u));

        // a dropped bag removes that player on the client
        Rpc.Dispatch("sb:drop", 0u, true, new[] { Value.String("player:4") });
        check.That("client removes a dropped player", !Players.IsConnected(4u));
        check.That("client raised PlayerLeft", left.Contains(4u));

        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
    }
}
