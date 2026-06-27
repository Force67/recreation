using System;

namespace Recreation.Net;

// The admin subsystem's orchestrator. Permissions and Commands hold the policy;
// Admin wires them for a role, registers the built-in commands, and exposes the
// action hooks (kick, announce) the runtime fills in.
public static class Admin
{
    // The effects built-in commands invoke. Default to no-ops so the system runs in
    // tests and headless tools without an engine; the runtime swaps in real ones.
    private static Action<uint> _kick = static _ => { };
    private static Action<string> _announce = static _ => { };

    // The runtime supplies the real kick (drop the peer). Null restores the no-op.
    public static void SetKickHandler(Action<uint> handler) => _kick = handler ?? (static _ => { });

    // The runtime supplies the real announce: broadcast the text to every player.
    public static void SetAnnounceHandler(Action<string> handler) =>
        _announce = handler ?? (static _ => { });

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
        _kick = static _ => { };
        _announce = static _ => { };
        Commands.Reset();
        Permissions.Reset();
    }

    // The privileged commands every server ships with. Each names the ACE its
    // caller must hold; the permission check happens in Commands before dispatch.
    private static void RegisterBuiltins()
    {
        Commands.Register("kick", "command.kick", ctx =>
        {
            if (ctx.Args.Length >= 1 && uint.TryParse(ctx.Args[0], out uint id))
            {
                _kick(id);
                ctx.Reply($"Kicked {id}");
            }
            else
            {
                ctx.Reply("usage: kick <id>");
            }
        });

        Commands.Register("announce", "command.announce", ctx =>
        {
            // The whole tail is the message, so multi-word announcements work.
            _announce(string.Join(' ', ctx.Args));
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
