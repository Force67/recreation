using System;
using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Modding;

// An inbound RPC delivered to a handler: who sent it and the arguments. Sender is
// the peer id (0 from the server); FromServer distinguishes a server push from a
// client send so authority checks are trivial.
public readonly struct RpcEvent
{
    public readonly uint Sender;
    public readonly bool FromServer;
    public readonly Value[] Args;

    public RpcEvent(uint sender, bool fromServer, Value[] args)
    {
        Sender = sender;
        FromServer = fromServer;
        Args = args;
    }
}

// The mod-facing multiplayer RPC surface. A client sends with Emit; the server
// answers a peer with ToClient or everyone with Broadcast; either side receives
// with On. The first On(name) tells the engine to start forwarding that name.
//
// Unbound in single-player (no RpcBridge in the handshake): outbound calls are
// no-ops and nothing is ever dispatched, so mods need no per-mode branching.
//
// Single-threaded: the engine calls Dispatch on the main thread, the same one
// mods send from, so the handler table needs no locking.
public static partial class Rpc
{
    private static IRpcBackend? _backend;
    private static readonly Dictionary<string, Action<RpcEvent>> Handlers = new();

    // client -> host
    public static void Emit(string name, params Value[] args) =>
        _backend?.Emit(RpcTarget.ToServer, 0, name, args);

    // host -> one client
    public static void ToClient(uint peer, string name, params Value[] args) =>
        _backend?.Emit(RpcTarget.ToClient, peer, name, args);

    // host -> all clients
    public static void Broadcast(string name, params Value[] args) =>
        _backend?.Emit(RpcTarget.Broadcast, 0, name, args);

    // Register a handler for an RPC name. The first handler for a name subscribes
    // with the engine so it begins forwarding; re-registering replaces the handler
    // without subscribing again.
    public static void On(string name, Action<RpcEvent> handler)
    {
        bool isNew = !Handlers.ContainsKey(name);
        Handlers[name] = handler;
        if (isNew) _backend?.Subscribe(name);
    }

    public static void Off(string name) => Handlers.Remove(name);

    // Drop every handler. ModHost.Shutdown calls this so subscriptions do not leak
    // across sessions.
    public static void Clear()
    {
        Handlers.Clear();
        ClearRequests();  // request/response state lives in the partial below
    }

    // Implemented in RpcRequest.cs; clears the pending request/response tables.
    static partial void ClearRequests();

    internal static void Bind(IRpcBackend backend) => _backend = backend;

    // Called by the host when an inbound RPC arrives. Routes it to the handler for
    // the name; an unsubscribed name is ignored.
    internal static void Dispatch(string name, uint sender, bool fromServer, ReadOnlySpan<Value> args)
    {
        if (!Handlers.TryGetValue(name, out Action<RpcEvent>? handler)) return;
        handler(new RpcEvent(sender, fromServer, args.ToArray()));
    }
}
