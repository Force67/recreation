using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Server-authoritative networked text chat. A client asks the host to say a line;
// the host decides who hears it. Routing rides two reserved RPC names:
//
//   * chat:say  client -> host: an outgoing line
//   * chat:msg  host -> client: a delivered message
//
// Standalone collapses to the server path (the lone player is authoritative).
public static partial class Chat
{
    private const string SayRpc = "chat:say";  // client -> host: an outgoing line
    private const string MsgRpc = "chat:msg";  // host -> client: a delivered message

    private static NetRole _role = NetRole.Standalone;
    private static readonly List<Action<ChatMessage>> Observers = new();

    // Decides whether a non-Global message reaches a recipient. Default accepts
    // everyone, so Team and Proximity behave like Global until a mod narrows them.
    private static Func<uint, uint, ChatChannel, bool> _recipientFilter = DefaultRecipientFilter;

    public static NetRole Role => _role;

    // Wire chat for a role. Idempotent (re-binding resets first).
    public static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        switch (role)
        {
            case NetRole.Server:
                Rpc.On(SayRpc, OnClientSay);
                RegisterBuiltins();
                break;
            case NetRole.Client:
                Rpc.On(MsgRpc, OnServerMsg);
                break;
            case NetRole.Standalone:
                // Authoritative over itself: Send routes through the server path.
                RegisterBuiltins();
                break;
        }
    }

    // Drop every observer, command and the recipient filter. Called on teardown.
    public static void Reset()
    {
        Observers.Clear();
        Commands.Clear();
        _recipientFilter = DefaultRecipientFilter;
        _role = NetRole.Standalone;
    }

    // Observe messages delivered on this machine. Host fires for what it
    // broadcasts/echoes to itself; a client fires on receipt.
    public static IDisposable OnMessage(Action<ChatMessage> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        Observers.Add(handler);
        return new Unsubscriber(() => Observers.Remove(handler));
    }

    // Narrow (or widen) who hears non-Global channels. Passing null restores the
    // accept-all default.
    public static void SetRecipientFilter(Func<uint, uint, ChatChannel, bool> filter) =>
        _recipientFilter = filter ?? DefaultRecipientFilter;

    public static void Send(string text) => Send(ChatChannel.Global, text);

    // The local player says a line. A client forwards to the host; the host (or
    // standalone) routes it immediately.
    public static void Send(ChatChannel channel, string text)
    {
        if (string.IsNullOrEmpty(text)) return;
        if (_role == NetRole.Client)
        {
            Rpc.Emit(SayRpc, Value.Int((int)channel), Value.String(text));
            return;
        }
        RouteIncoming(Players.LocalId, channel, text);
    }

    // Server-side injection of a message, delivered to everyone. No-op on a client.
    public static void Post(ChatMessage msg)
    {
        if (_role == NetRole.Client) return;
        Deliver(msg);
    }

    // Convenience: broadcast a System line with no human sender.
    public static void System(string text) =>
        Post(new ChatMessage(0, string.Empty, ChatChannel.System, text));

    // Host: a client's outgoing line arrived. Channel and text are the wire args.
    private static void OnClientSay(RpcEvent e)
    {
        if (e.Args.Length < 2) return;
        RouteIncoming(e.Sender, (ChatChannel)e.Args[0].AsInt(), e.Args[1].AsString());
    }

    // Host: handle one inbound line. A leading slash is a command; otherwise a
    // chat message stamped with the sender's display name.
    private static void RouteIncoming(uint sender, ChatChannel channel, string text)
    {
        if (text.StartsWith('/'))
        {
            DispatchCommand(sender, text);
            return;
        }
        string name = Players.Get(sender)?.Name ?? $"Player {sender}";
        Deliver(new ChatMessage(sender, name, channel, text));
    }

    // Host: deliver a message to its recipients. Global and System reach everyone
    // (broadcast plus the host's own copy); Team and Proximity honour the recipient
    // filter, always including the sender so a player sees their own line.
    private static void Deliver(ChatMessage msg)
    {
        if (msg.Channel is ChatChannel.Global or ChatChannel.System)
        {
            Rpc.Broadcast(MsgRpc, Pack(msg));
            DeliverLocal(msg);
            return;
        }
        foreach (Player p in Players.All)
        {
            bool accept = p.Id == msg.Sender || _recipientFilter(msg.Sender, p.Id, msg.Channel);
            if (!accept) continue;
            if (p.Id == Players.LocalId) DeliverLocal(msg);
            else Rpc.ToClient(p.Id, MsgRpc, Pack(msg));
        }
    }

    // Client: the host delivered a message. Reconstruct and surface it locally.
    private static void OnServerMsg(RpcEvent e)
    {
        if (e.Args.Length < 5) return;
        DeliverLocal(Unpack(e.Args));
    }

    // Surface a message locally: notify observers, then draw on the HUD. The HUD
    // native is no-op-safe (returns None when no HUD is wired).
    private static void DeliverLocal(ChatMessage msg)
    {
        Raise(msg);
        Native.CallGlobal("Hud", "ChatLine",
            new[] { Value.String(msg.SenderName), Value.String(msg.Text), Value.Int((int)msg.Color) });
    }

    private static void Raise(ChatMessage msg)
    {
        if (Observers.Count == 0) return;
        // Snapshot so an observer may (un)subscribe during dispatch.
        Action<ChatMessage>[] snapshot = Observers.ToArray();
        foreach (Action<ChatMessage> h in snapshot)
        {
            try { h(msg); }
            catch (Exception ex) { Console.Error.WriteLine($"[chat] observer threw: {ex.Message}"); }
        }
    }

    // The chat:msg wire layout, shared by Deliver (host) and OnServerMsg (client).
    private static Value[] Pack(ChatMessage m) => new[]
    {
        Value.Int((int)m.Sender),
        Value.String(m.SenderName),
        Value.Int((int)m.Channel),
        Value.String(m.Text),
        Value.Int((int)m.Color),
    };

    private static ChatMessage Unpack(Value[] a) => new(
        (uint)a[0].AsInt(),
        a[1].AsString(),
        (ChatChannel)a[2].AsInt(),
        a[3].AsString(),
        (uint)a[4].AsInt());

    private static bool DefaultRecipientFilter(uint sender, uint recipient, ChatChannel channel) => true;
}
