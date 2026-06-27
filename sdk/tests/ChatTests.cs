using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the server-authoritative chat layer across roles: routing, slash commands, System posts, and client receive.
public static class ChatTests
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
        // A bound backend keeps the HUD render path (DeliverLocal -> CallGlobal) from
        // throwing; it returns None for the unwired ChatLine native.
        Native.Backend = new FakeBackend();

        // --- standalone: a line is routed and delivered locally, no real network ---
        Rpc.Clear();
        Rpc.Bind(new Recording());
        Platform.Boot(NetRole.Standalone);
        Chat.Bind(NetRole.Standalone);

        int fired = 0;
        ChatMessage seen = default;
        IDisposable sub = Chat.OnMessage(m => { fired++; seen = m; });
        Chat.Send("hi");
        check.Equal("standalone delivers locally", 1, fired);
        check.Equal("standalone message text", "hi", seen.Text);
        check.Equal("standalone sender is the local player", Players.LocalId, seen.Sender);
        check.Equal("standalone defaults to the global channel", ChatChannel.Global, seen.Channel);
        sub.Dispose();
        Chat.Send("ignored");
        check.Equal("disposed observer stops firing", 1, fired);

        // --- server: a client's line is resolved, broadcast and delivered locally ---
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Server);
        Chat.Bind(NetRole.Server);

        EventBus.Publish(new ClientJoined(5u));   // admit the peer so its name resolves
        Players.Get(5u)!.SetName("Bob");

        int srvFired = 0;
        ChatMessage srvSeen = default;
        Chat.OnMessage(m => { srvFired++; srvSeen = m; });

        rec.Emits.Clear();
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("hello") });
        check.Equal("server delivers the message locally", 1, srvFired);
        check.Equal("server resolves the sender's name", "Bob", srvSeen.SenderName);
        check.Equal("server message text", "hello", srvSeen.Text);
        check.Equal("global message is broadcast", "chat:msg", rec.Emits[^1].Name);
        check.Equal("broadcast to everyone", RpcTarget.Broadcast, rec.Emits[^1].Target);
        check.Equal("broadcast carries the sender id", 5, rec.Emits[^1].Args[0].AsInt());
        check.Equal("broadcast carries the resolved name", "Bob", rec.Emits[^1].Args[1].AsString());
        check.Equal("broadcast carries the text", "hello", rec.Emits[^1].Args[3].AsString());

        // --- command: a registered slash command dispatches with parsed args ---
        uint cmdSender = 0;
        string[] cmdArgs = Array.Empty<string>();
        string cmdRaw = "";
        Chat.RegisterCommand("foo", ctx =>
        {
            cmdSender = ctx.Sender;
            cmdArgs = ctx.Args;
            cmdRaw = ctx.Raw;
        });
        rec.Emits.Clear();
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("/foo a b") });
        check.Equal("command sender is the caller", 5u, cmdSender);
        check.Equal("command parses two args", 2, cmdArgs.Length);
        check.Equal("command arg 0", "a", cmdArgs.Length > 0 ? cmdArgs[0] : "");
        check.Equal("command arg 1", "b", cmdArgs.Length > 1 ? cmdArgs[1] : "");
        check.Equal("command keeps the raw line", "/foo a b", cmdRaw);
        check.That("a handled command does not broadcast",
            !rec.Emits.Exists(e => e.Target == RpcTarget.Broadcast));

        // --- command: an unknown command replies only to the sender ---
        rec.Emits.Clear();
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("/nope") });
        check.Equal("unknown command replies", "chat:msg", rec.Emits[^1].Name);
        check.Equal("reply is unicast to the sender", RpcTarget.ToClient, rec.Emits[^1].Target);
        check.Equal("reply targets the sender peer", 5u, rec.Emits[^1].Peer);
        check.That("reply names the unknown command", rec.Emits[^1].Args[3].AsString().Contains("nope"));
        check.That("unknown command does not broadcast",
            !rec.Emits.Exists(e => e.Target == RpcTarget.Broadcast));

        // --- command: /help lists the registered commands ---
        rec.Emits.Clear();
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("/help") });
        string helpText = rec.Emits[^1].Args[3].AsString();
        check.That("help lists the user command", helpText.Contains("foo"));
        check.That("help lists the built-in help", helpText.Contains("help"));

        // --- post/system: an injected System line is broadcast and delivered ---
        Rpc.Clear();
        rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Server);
        Chat.Bind(NetRole.Server);

        int sysFired = 0;
        ChatMessage sysSeen = default;
        Chat.OnMessage(m => { sysFired++; sysSeen = m; });
        rec.Emits.Clear();
        Chat.System("server restarting");
        check.Equal("system message delivered locally", 1, sysFired);
        check.Equal("system message uses the system channel", ChatChannel.System, sysSeen.Channel);
        check.Equal("system message text", "server restarting", sysSeen.Text);
        check.Equal("system message is broadcast", "chat:msg", rec.Emits[^1].Name);
        check.Equal("system broadcast to everyone", RpcTarget.Broadcast, rec.Emits[^1].Target);

        // --- client: a send forwards to the host and a received line is surfaced ---
        Rpc.Clear();
        rec = new Recording();
        Rpc.Bind(rec);
        var fake = new FakeBackend();
        Native.Backend = fake;
        Platform.Boot(NetRole.Client);
        Chat.Bind(NetRole.Client);

        int cliFired = 0;
        ChatMessage cliSeen = default;
        Chat.OnMessage(m => { cliFired++; cliSeen = m; });

        Chat.Send("hey");
        check.Equal("client send forwards to the host", "chat:say", rec.Emits[^1].Name);
        check.Equal("client send targets the server", RpcTarget.ToServer, rec.Emits[^1].Target);
        check.Equal("client send does not deliver locally", 0, cliFired);

        Rpc.Dispatch("chat:msg", 0u, true,
            new[] { Value.Int(7), Value.String("Lydia"), Value.Int(0), Value.String("greetings"), Value.Int(0) });
        check.Equal("client surfaces a received message", 1, cliFired);
        check.Equal("received sender id", 7u, cliSeen.Sender);
        check.Equal("received sender name", "Lydia", cliSeen.SenderName);
        check.Equal("received text", "greetings", cliSeen.Text);
        check.Equal("client renders on the HUD", "Hud", fake.LastGlobalType);
        check.Equal("client renders a chat line", "ChatLine", fake.LastGlobalFunction);

        // --- reset: handlers and commands are cleared ---
        bool clearedRan = false;
        int clearedFired = 0;
        Chat.RegisterCommand("zzz", _ => clearedRan = true);
        Chat.OnMessage(_ => clearedFired++);
        Chat.Reset();
        check.Equal("reset reverts the role to standalone", NetRole.Standalone, Chat.Role);

        Rpc.Clear();
        rec = new Recording();
        Rpc.Bind(rec);
        Native.Backend = new FakeBackend();
        Platform.Boot(NetRole.Server);
        Chat.Bind(NetRole.Server);
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("/zzz") });
        check.That("reset cleared user commands", !clearedRan);
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("hello again") });
        check.Equal("reset cleared observers", 0, clearedFired);

        Chat.Reset();
        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
