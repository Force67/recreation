using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Exercises the managed RPC surface against a recording backend: the first On
// subscribes once, the outbound helpers carry the right target/peer/name/args,
// and Dispatch routes inbound RPCs to the matching handler. The native RpcBridge
// boundary is covered by the pexrun hosttest instead.
public static class RpcTests
{
    // A fake IRpcBackend that records every emit and subscription, standing in for
    // the network so the SDK runs without a live engine.
    private sealed class RecordingBackend : IRpcBackend
    {
        public readonly List<(RpcTarget Target, uint Peer, string Name, Value[] Args)> Emits = new();
        public readonly List<string> Subscriptions = new();

        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args) =>
            Emits.Add((target, peer, name, args.ToArray()));

        public void Subscribe(string name) => Subscriptions.Add(name);
    }

    // Loops every emit straight back into Dispatch, simulating the peer on the
    // other end so request/response can be exercised end to end in one process: a
    // client send arrives as from-a-client, a server push as from-the-server.
    private sealed class LoopbackBackend : IRpcBackend
    {
        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args)
        {
            bool fromServer = target != RpcTarget.ToServer;
            uint sender = target == RpcTarget.ToServer ? 7u : 0u;
            Rpc.Dispatch(name, sender, fromServer, args);
        }

        public void Subscribe(string name) { }
    }

    public static void Run(Check check)
    {
        var backend = new RecordingBackend();
        Rpc.Clear();
        Rpc.Bind(backend);

        // First On subscribes; re-On for the same name does not subscribe again.
        Rpc.On("rt_event", _ => { });
        Rpc.On("rt_event", _ => { });
        check.Equal("first On subscribes once", 1, backend.Subscriptions.Count);
        check.Equal("re-On does not re-subscribe", "rt_event",
            backend.Subscriptions.Count == 1 ? backend.Subscriptions[0] : "");

        // Emit -> ToServer with peer 0.
        Rpc.Emit("rt_send", Value.Int(7));
        check.Equal("Emit target", RpcTarget.ToServer, backend.Emits[^1].Target);
        check.Equal("Emit peer", 0u, backend.Emits[^1].Peer);
        check.Equal("Emit name", "rt_send", backend.Emits[^1].Name);
        check.Equal("Emit arg", 7, backend.Emits[^1].Args[0].AsInt());

        // ToClient -> a single peer.
        Rpc.ToClient(42, "rt_push", Value.String("hi"));
        check.Equal("ToClient target", RpcTarget.ToClient, backend.Emits[^1].Target);
        check.Equal("ToClient peer", 42u, backend.Emits[^1].Peer);
        check.Equal("ToClient name", "rt_push", backend.Emits[^1].Name);
        check.Equal("ToClient arg", "hi", backend.Emits[^1].Args[0].AsString());

        // Broadcast -> all clients, peer 0.
        Rpc.Broadcast("rt_all", Value.Bool(true), Value.Float(1.5f));
        check.Equal("Broadcast target", RpcTarget.Broadcast, backend.Emits[^1].Target);
        check.Equal("Broadcast peer", 0u, backend.Emits[^1].Peer);
        check.Equal("Broadcast arg count", 2, backend.Emits[^1].Args.Length);
        check.That("Broadcast arg0", backend.Emits[^1].Args[0].AsBool());
        check.Equal("Broadcast arg1", 1.5f, backend.Emits[^1].Args[1].AsFloat());

        // Dispatch invokes the matching handler with the right sender/origin/args.
        uint gotSender = 0;
        bool gotServer = false;
        int gotArg = 0;
        Rpc.On("rt_in", e =>
        {
            gotSender = e.Sender;
            gotServer = e.FromServer;
            gotArg = e.Args[0].AsInt();
        });
        Rpc.Dispatch("rt_in", 9, true, new[] { Value.Int(123) });
        check.Equal("Dispatch sender", 9u, gotSender);
        check.That("Dispatch fromServer", gotServer);
        check.Equal("Dispatch arg", 123, gotArg);

        // An unknown name is ignored (no throw, nothing fires).
        Rpc.Dispatch("rt_unknown", 1, false, ReadOnlySpan<Value>.Empty);
        check.Equal("unknown dispatch does not fire", 123, gotArg);

        // Off removes a handler; Dispatch then no-ops for that name.
        int firedAfterOff = 0;
        Rpc.On("rt_off", _ => firedAfterOff++);
        Rpc.Off("rt_off");
        Rpc.Dispatch("rt_off", 0, false, ReadOnlySpan<Value>.Empty);
        check.Equal("Off removes the handler", 0, firedAfterOff);

        // Clear drops everything left.
        int firedAfterClear = 0;
        Rpc.On("rt_clear", _ => firedAfterClear++);
        Rpc.Clear();
        Rpc.Dispatch("rt_clear", 0, false, ReadOnlySpan<Value>.Empty);
        check.Equal("Clear removes handlers", 0, firedAfterClear);

        // --- request / response over the loopback ---
        Rpc.Clear();
        Rpc.Bind(new LoopbackBackend());

        string serverGot = "";
        Rpc.OnRequest("rt_buy", req =>
        {
            serverGot = req.Args[0].AsString();
            req.Reply(Value.Bool(true), Value.Int(99));
        });
        bool replied = false;
        bool allowed = false;
        int change = 0;
        Rpc.Request("rt_buy", new[] { Value.String("sword") }, reply =>
        {
            replied = true;
            allowed = reply[0].AsBool();
            change = reply[1].AsInt();
        });
        check.That("request reached the handler", serverGot == "sword");
        check.That("reply delivered to the caller", replied);
        check.That("reply payload arrives", allowed);
        check.Equal("reply second value", 99, change);

        // A request with no matching handler simply never replies (no throw).
        bool orphanReplied = false;
        Rpc.Request("rt_nohandler", Array.Empty<Value>(), _ => orphanReplied = true);
        check.That("unhandled request does not reply", !orphanReplied);

        // Clear wipes pending requests and request handlers too.
        Rpc.Clear();
        Rpc.Bind(new LoopbackBackend());
        bool afterClear = false;
        Rpc.Request("rt_buy", Array.Empty<Value>(), _ => afterClear = true);
        check.That("cleared request handler does not answer", !afterClear);

        // A request to a peer that leaves is dropped, so its callback cannot linger.
        // The recording backend never replies, so the requests stay pending.
        Rpc.Clear();
        Rpc.Bind(new RecordingBackend());
        Rpc.RequestClient(5, "rt_q", Array.Empty<Value>(), _ => { });
        Rpc.RequestClient(6, "rt_q", Array.Empty<Value>(), _ => { });
        check.Equal("two requests pending", 2, Rpc.PendingRequestCount);
        Rpc.DropPeerRequests(5);
        check.Equal("the leaving peer's request is dropped", 1, Rpc.PendingRequestCount);
        Rpc.DropPeerRequests(6);
        check.Equal("the other peer's request is dropped", 0, Rpc.PendingRequestCount);
        Rpc.Clear();
    }
}
