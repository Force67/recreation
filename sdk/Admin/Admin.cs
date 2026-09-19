using System;
using Recreation.Interop;

namespace Recreation.Net;

// The admin subsystem's orchestrator. Permissions and Commands hold the policy;
// Admin wires them for a role, registers the built-in commands, and exposes the
// action hooks (kick, announce) the runtime fills in.
public static class Admin
{
    // The effects built-in commands invoke. They default to the real thing rather
    // than to no-ops: a kick that reports success and drops nobody is worse than
    // no kick at all. Kicking goes through the engine, which owns the transport
    // that can disconnect somebody; announcing is a chat broadcast, which is
    // already ours. A host can still replace either.
    private static Action<uint> _kick = DefaultKick;
    private static Action<string> _announce = DefaultAnnounce;

    // Drops the peer. No-op off a host, or with no session (single player).
    private static void DefaultKick(uint peer) =>
        Native.CallGlobal("Net", "Kick", new[] { Value.Int((int)peer) });

    private static void DefaultAnnounce(string text) => Chat.System(text);

    // Replaces the kick. Null restores the engine's own.
    public static void SetKickHandler(Action<uint> handler) => _kick = handler ?? DefaultKick;

    // Replaces the announce. Null restores the chat broadcast.
    public static void SetAnnounceHandler(Action<string> handler) =>
        _announce = handler ?? DefaultAnnounce;

    // Bring the admin layer up for a role. Idempotent. On the authoritative side it
    // registers the built-in commands and seeds the host as admin.
    public static void Bind(NetRole role)
    {
        Reset();
        Commands.Bind(role);
        if (role != NetRole.Client)  // server or standalone owns the policy
        {
            RegisterBuiltins();
            SeedHostAdmin();
        }
    }

    // Tear everything down: hooks back to no-op, commands and permissions cleared.
    public static void Reset()
    {
        _kick = DefaultKick;
        _announce = DefaultAnnounce;
        Commands.Reset();
        Permissions.Reset();
    }

    // The privileged commands every server ships with. Each names the ACE its
    // caller must hold; the permission check happens in Commands before dispatch.
    private static void RegisterBuiltins()
    {
        Commands.Register("kick", "command.kick", ctx =>
        {
            if (ctx.Args.Length < 1 || !uint.TryParse(ctx.Args[0], out uint id))
            {
                ctx.Reply("usage: kick <id>");
                return;
            }
            // Checked against the roster first: the engine's kick is
            // fire-and-forget, so this is the only place that can tell an admin
            // they just kicked nobody instead of reporting a kick that did not
            // happen.
            if (!Players.IsConnected(id))
            {
                ctx.Reply($"nobody with id {id} is connected");
                return;
            }
            _kick(id);
            ctx.Reply($"Kicked {id}");
        });

        Commands.Register("announce", "command.announce", ctx =>
        {
            // The whole tail is the message, so multi-word announcements work.
            string text = string.Join(' ', ctx.Args);
            if (string.IsNullOrWhiteSpace(text))
            {
                ctx.Reply("usage: announce <message>");
                return;
            }
            _announce(text);
            ctx.Reply($"announced: {text}");
        });

        Commands.Register("setgroup", "command.setgroup", ctx =>
        {
            if (ctx.Args.Length >= 2 && uint.TryParse(ctx.Args[0], out uint id))
            {
                Permissions.AddPrincipalToGroup(Permissions.PlayerPrincipal(id), $"group.{ctx.Args[1]}");
                ctx.Reply($"Added {id} to group.{ctx.Args[1]}");
            }
            else
            {
                ctx.Reply("usage: setgroup <id> <group>");
            }
        });
    }

    // Seed the session owner (peer 0) as an admin so the host operator can run
    // privileged commands immediately. group.admin is granted everything ("*").
    private static void SeedHostAdmin()
    {
        Permissions.AddAce("group.admin", "*", true);
        Permissions.AddPrincipalToGroup(Permissions.PlayerPrincipal(0), "group.admin");
    }
}
