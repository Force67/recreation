using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The slash-command half of chat. Commands are host-side: only the host (or
// standalone) holds the registry and runs handlers. A handler answers its caller
// through the context's Reply (a private System line to just that sender).
public static partial class Chat
{
    // Command name (without the slash) -> handler. Case-insensitive so "/Help" and
    // "/help" are the same command.
    private static readonly Dictionary<string, Action<ChatCommandContext>> Commands =
        new(StringComparer.OrdinalIgnoreCase);

    // Register (or replace) a host-side command. The name is given without the
    // leading slash; players invoke it as "/<name>".
    public static void RegisterCommand(string name, Action<ChatCommandContext> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        Commands[name] = handler;
    }

    public static bool HasCommand(string name) => Commands.ContainsKey(name);

    // Host: parse and run a slash command from a sender. A bare "/" is ignored; an
    // unknown command replies only to the sender, so a typo never spams the server.
    private static void DispatchCommand(uint sender, string raw)
    {
        string[] parts = raw[1..].Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0) return;
        string name = parts[0];
        string[] args = parts.Length > 1 ? parts[1..] : Array.Empty<string>();
        var ctx = new ChatCommandContext(sender, args, raw);
        if (!Commands.TryGetValue(name, out Action<ChatCommandContext>? handler))
        {
            ctx.Reply($"Unknown command: {name}");
            return;
        }
        // Isolate a throwing handler so one bad command cannot take down routing.
        try { handler(ctx); }
        catch (Exception ex) { Console.Error.WriteLine($"[chat] command '{name}' threw: {ex.Message}"); }
    }

    // Host: send a private System line to one peer. Delivered locally when that peer
    // is the host itself, else unicast to the client.
    internal static void ReplyTo(uint peer, string text)
    {
        var msg = new ChatMessage(0, string.Empty, ChatChannel.System, text);
        if (peer == Players.LocalId) DeliverLocal(msg);
        else Rpc.ToClient(peer, MsgRpc, Pack(msg));
    }

    // The commands every session ships with. Registered in Bind(Server/Standalone).
    private static void RegisterBuiltins()
    {
        RegisterCommand("help", ctx =>
        {
            var names = new List<string>(Commands.Keys);
            names.Sort(StringComparer.Ordinal);
            ctx.Reply("Commands: " + string.Join(", ", names));
        });
    }
}

// What a command handler receives. Args excludes the command name; Raw is the full
// "/name a b" as typed for handlers that want to reparse it.
public sealed class ChatCommandContext
{
    public uint Sender { get; }
    public string[] Args { get; }
    public string Raw { get; }

    internal ChatCommandContext(uint sender, string[] args, string raw)
    {
        Sender = sender;
        Args = args;
        Raw = raw;
    }

    // Answer the caller with a private System line.
    public void Reply(string text) => Chat.ReplyTo(Sender, text);
}
