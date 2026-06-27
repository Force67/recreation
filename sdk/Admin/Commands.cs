using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// What a command handler is handed when it runs. Reply reaches just that sender.
public sealed class CommandContext
{
    public uint Sender { get; }
    public string[] Args { get; }
    public string Raw { get; }

    private readonly Action<string> _reply;

    internal CommandContext(uint sender, string[] args, string raw, Action<string> reply)
    {
        Sender = sender;
        Args = args;
        Raw = raw;
        _reply = reply;
    }

    // Send a line back to the player who ran the command (and only them).
    public void Reply(string text) => _reply(text);
}

// The privileged admin-command registry, for actions gated behind permissions
// (kick, announce, ...). A client invokes a command with Run; it travels to the
// server, which checks the caller's ACE before dispatching. The host runs its own
// commands through the same gate, so authority is enforced in one place.
public static class Commands
{
    private const string CmdRpc = "admin:cmd";     // client -> server: run this command
    private const string ReplyRpc = "admin:reply"; // server -> a client: command feedback

    // name -> (the ACE the caller must hold, the handler). Case-insensitive so
    // "Kick" and "kick" are the same command.
    private static readonly Dictionary<string, Entry> Registry =
        new(StringComparer.OrdinalIgnoreCase);

    // Register a server command. Handlers run on the host after the caller passes
    // the permission check.
    public static void Register(string name, string requiredAce, Action<CommandContext> handler)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentNullException.ThrowIfNull(handler);
        Registry[name] = new Entry(requiredAce, handler);
    }

    // Invoke a command. A client forwards it to the host; the host (and standalone)
    // runs it locally as the local player.
    public static void Run(string name, params string[] args)
    {
        if (Platform.IsClient)
        {
            // Pack as [name, arg0, arg1, ...] so the server can split it back out.
            Value[] payload = new Value[args.Length + 1];
            payload[0] = Value.String(name);
            for (int i = 0; i < args.Length; i++) payload[i + 1] = Value.String(args[i]);
            Rpc.Emit(CmdRpc, payload);
            return;
        }
        Execute(name, Players.LocalId, args);
    }

    // Wire the RPC handlers for a role. Idempotent (resets first).
    internal static void Bind(NetRole role)
    {
        Reset();
        switch (role)
        {
            case NetRole.Server:
                Rpc.On(CmdRpc, OnServerCmd);
                break;
            case NetRole.Client:
                Rpc.On(ReplyRpc, OnClientReply);
                break;
            case NetRole.Standalone:
                break;  // no network: Run dispatches locally
        }
    }

    // Forget every registered command. Called by Admin.Reset.
    internal static void Reset() => Registry.Clear();

    // Server: a client asked to run a command. Split out the name and arguments and
    // dispatch under that peer's identity.
    private static void OnServerCmd(RpcEvent e)
    {
        if (e.Args.Length < 1) return;
        string name = e.Args[0].AsString();
        string[] args = new string[e.Args.Length - 1];
        for (int i = 1; i < e.Args.Length; i++) args[i - 1] = e.Args[i].AsString();
        Execute(name, e.Sender, args);
    }

    // Client: the server sent command feedback. Surface it on the local console.
    private static void OnClientReply(RpcEvent e)
    {
        if (e.Args.Length >= 1) Console.WriteLine($"[admin] {e.Args[0].AsString()}");
    }

    // The single dispatch point: enforce the ACE, then run the handler. Unknown
    // commands are ignored (no surface for probing which commands exist).
    private static void Execute(string name, uint sender, string[] args)
    {
        if (!Registry.TryGetValue(name, out Entry? entry)) return;
        if (!Permissions.IsPlayerAllowed(sender, entry.RequiredAce))
        {
            Reply(sender, "Access denied");
            return;
        }
        string raw = args.Length == 0 ? name : $"{name} {string.Join(' ', args)}";
        entry.Handler(new CommandContext(sender, args, raw, text => Reply(sender, text)));
    }

    // Feedback to a single caller: locally when it is the host operator, otherwise
    // over the wire to that peer.
    private static void Reply(uint sender, string text)
    {
        if (sender == Players.LocalId) Console.WriteLine($"[admin] {text}");
        else Rpc.ToClient(sender, ReplyRpc, Value.String(text));
    }

    private sealed record Entry(string RequiredAce, Action<CommandContext> Handler);
}
