using System;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The managed half of a dedicated server's operator console. The engine owns the
// terminal and answers for what only it knows (status, convars, the clock); every
// other line arrives here, where the platform's own command registry lives.
//
// A console line runs as the host operator (peer 0, seeded into group.admin), so
// it passes every permission check and its replies print to the terminal through
// the same path an in-game admin's do. That is deliberate: whoever can type into
// the server's terminal already owns the server.
public static class ServerConsole
{
    // Mirrors kConsoleRpcName in runtime/app/server_console.h.
    private const string ConsoleRpc = "rx:console";

    private static bool _bound;

    // Bring the console's command surface up for a role. A client has no console;
    // it would only be a way to run privileged commands nobody granted.
    public static void Bind(NetRole role)
    {
        Reset();
        if (role == NetRole.Client) return;
        Rpc.On(ConsoleRpc, OnLine);
        RegisterBuiltins();
        _bound = true;
    }

    public static void Reset() => _bound = false;

    // A line the engine had no command for. Whatever leads it is a command name;
    // the rest are its arguments.
    private static void OnLine(RpcEvent e)
    {
        if (!_bound || e.Args.Length < 1) return;
        string[] parts = e.Args[0].AsString()
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0) return;
        string name = parts[0];
        string[] args = parts.Length > 1 ? parts[1..] : Array.Empty<string>();
        if (!Commands.Has(name))
        {
            // The engine cannot know what mods registered, so this is the only
            // place that can say a command does not exist at all.
            Console.WriteLine($"[console] unknown command: {name}");
            return;
        }
        Commands.Run(name, args);
    }

    // The commands a console expects to have, registered here rather than in
    // Admin's built-ins because they exist to be typed at a terminal. Each still
    // names an ACE, so an in-game admin holding it can run them too.
    private static void RegisterBuiltins()
    {
        Commands.Register("players", "command.players", ctx =>
        {
            if (Players.Count == 0)
            {
                ctx.Reply("nobody connected");
                return;
            }
            ctx.Reply($"{Players.Count} connected:");
            foreach (Player p in Players.All)
                ctx.Reply($"  [{p.Id}] {p.Name}");
        });

        // The console spelling of announce: the verb an operator reaches for at a
        // terminal, with a permission of its own so it can be granted separately.
        Commands.Register("say", "command.say", ctx =>
        {
            // The whole tail is the message, so multi-word lines work.
            string text = string.Join(' ', ctx.Args);
            if (string.IsNullOrWhiteSpace(text))
            {
                ctx.Reply("usage: say <message>");
                return;
            }
            Chat.System(text);
            ctx.Reply($"said: {text}");
        });
    }
}
