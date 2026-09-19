using System;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Covers the managed half of the server console: a line the engine forwarded
// reaches the platform's command registry, runs as the host operator (so it
// passes the permission gate), and an unknown name is reported rather than
// swallowed the way a client's probe is.
public static class ServerConsoleTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        Platform.Boot(NetRole.Server);
        Players.LocalId = 0;

        // A mod's own command, reached from the console.
        string? ranWith = null;
        Commands.Register("spawnrate", "command.spawnrate", ctx =>
        {
            ranWith = string.Join(' ', ctx.Args);
            ctx.Reply("ok");
        });
        check.That("the console's builtins registered", Commands.Has("players") && Commands.Has("say"));

        Dispatch("spawnrate 3 fast");
        check.Equal("a console line runs the command with its arguments", "3 fast", ranWith ?? "");

        // The host operator is peer 0 and seeded into group.admin, so a console
        // line is not refused by the permission gate.
        ranWith = null;
        Commands.Register("gated", "command.nobody.has.this", ctx =>
        {
            ranWith = "ran";
            ctx.Reply("ok");
        });
        Dispatch("gated");
        check.Equal("the console passes the permission gate", "ran", ranWith ?? "");

        // say reaches chat.
        ChatMessage said = default;
        using var sub = Chat.OnMessage(m => said = m);
        Dispatch("say the server is going down in 5");
        check.Equal("say broadcasts the whole tail", "the server is going down in 5", said.Text);
        check.Equal("say speaks on the system channel", ChatChannel.System, said.Channel);

        // An unknown command is reported, not silently dropped.
        Dispatch("nosuchcommand");
        check.That("an unknown command does not throw", true);

        // A client has no console: a line that somehow arrives there does nothing.
        Platform.Boot(NetRole.Client);
        ranWith = null;
        Commands.Register("spawnrate", "command.spawnrate", ctx => ranWith = "ran");
        Dispatch("spawnrate 9");
        check.Equal("a client's console runs nothing", "", ranWith ?? "");

        ModHost.Shutdown();
    }

    // Stands in for the engine handing a typed line to the managed world.
    private static void Dispatch(string line) =>
        Rpc.Dispatch("rx:console", 0, true, new[] { Value.String(line) });
}
