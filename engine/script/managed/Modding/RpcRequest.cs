using System;
using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Modding;

// A request awaiting a reply, handed to an OnRequest handler. The handler reads
// Args and calls Reply once; the reply travels back to whoever asked, matched to
// the original call. This is the common serverside-scripting shape: a client asks
// "may I buy this?" and the server answers authoritatively.
public readonly struct RpcRequest
{
    public readonly uint Sender;      // the peer that asked (0 = the host)
    public readonly bool FromServer;  // true when the host made the request
    public readonly Value[] Args;
    private readonly ulong _id;

    internal RpcRequest(uint sender, bool fromServer, ulong id, Value[] args)
    {
        Sender = sender;
        FromServer = fromServer;
        _id = id;
        Args = args;
    }

    // Sends the reply back to the requester. Call at most once per request.
    public void Reply(params Value[] reply)
    {
        var payload = new Value[reply.Length + 1];
        payload[0] = Value.Object(_id);
        Array.Copy(reply, 0, payload, 1, reply.Length);
        // The host answers a client; a client answers the host.
        if (FromServer) Rpc.Emit(Rpc.ReplyChannel, payload);
        else Rpc.ToClient(Sender, Rpc.ReplyChannel, payload);
    }
}

// Request/response over the fire-and-forget RPC primitives: a leading correlation
// id rides two reserved channels (@req / @rep) so a reply finds its caller. Kept
// in a partial so the request surface lives apart from the core Rpc transport.
public static partial class Rpc
{
    internal const string RequestChannel = "@req";
    internal const string ReplyChannel = "@rep";

    private static ulong _nextRequestId = 1;
    private static readonly Dictionary<ulong, Action<Value[]>> Pending = new();
    private static readonly Dictionary<string, Action<RpcRequest>> RequestHandlers = new();
    private static bool _requestsWired;

    // Client to host request expecting a reply. onReply runs when the answer
    // arrives (a dropped reply simply never fires; no timeout is imposed).
    public static void Request(string name, Value[] args, Action<Value[]> onReply) =>
        SendRequest(RpcTarget.ToServer, 0, name, args, onReply);

    // Host to one client request expecting a reply.
    public static void RequestClient(uint peer, string name, Value[] args, Action<Value[]> onReply) =>
        SendRequest(RpcTarget.ToClient, peer, name, args, onReply);

    // Registers a handler that answers requests of this name with request.Reply.
    public static void OnRequest(string name, Action<RpcRequest> handler)
    {
        WireRequests();
        RequestHandlers[name] = handler;
    }

    public static void OffRequest(string name) => RequestHandlers.Remove(name);

    private static void SendRequest(RpcTarget target, uint peer, string name, Value[] args,
                                    Action<Value[]> onReply)
    {
        WireRequests();
        ulong id = _nextRequestId++;
        Pending[id] = onReply;
        var payload = new Value[args.Length + 2];
        payload[0] = Value.Object(id);
        payload[1] = Value.String(name);
        Array.Copy(args, 0, payload, 2, args.Length);
        if (target == RpcTarget.ToServer) Emit(RequestChannel, payload);
        else ToClient(peer, RequestChannel, payload);
    }

    // Subscribes the internal routers the first time the request API is used, after
    // a backend is bound so the subscription actually reaches the engine.
    private static void WireRequests()
    {
        if (_requestsWired || _backend == null) return;
        _requestsWired = true;
        On(RequestChannel, HandleIncomingRequest);
        On(ReplyChannel, HandleIncomingReply);
    }

    private static void HandleIncomingRequest(RpcEvent e)
    {
        if (e.Args.Length < 2) return;
        ulong id = e.Args[0].AsHandle();
        string name = e.Args[1].AsString();
        if (!RequestHandlers.TryGetValue(name, out Action<RpcRequest>? handler)) return;
        var args = new Value[e.Args.Length - 2];
        Array.Copy(e.Args, 2, args, 0, args.Length);
        handler(new RpcRequest(e.Sender, e.FromServer, id, args));
    }

    private static void HandleIncomingReply(RpcEvent e)
    {
        if (e.Args.Length < 1) return;
        ulong id = e.Args[0].AsHandle();
        if (!Pending.TryGetValue(id, out Action<Value[]>? onReply)) return;
        Pending.Remove(id);
        var args = new Value[e.Args.Length - 1];
        Array.Copy(e.Args, 1, args, 0, args.Length);
        onReply(args);
    }

    static partial void ClearRequests()
    {
        Pending.Clear();
        RequestHandlers.Clear();
        _requestsWired = false;
    }
}
