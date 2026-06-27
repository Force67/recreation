using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the admin subsystem: ACE model, permission-gated commands, kick/announce hooks, and Reset.
public static class AdminTests
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
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Server);
        Admin.Bind(NetRole.Server);

        // --- ACE basics (use fresh principals so seeded host admin can't interfere) ---
        Permissions.AddAce("player.5", "command.kick", true);
        check.That("direct allow grants the object", Permissions.IsAllowed("player.5", "command.kick"));
        check.That("unlisted object is denied by default",
            !Permissions.IsAllowed("player.5", "command.announce"));

        // Deny overrides a broader allow.
        Permissions.AddAce("player.5", "command.*", true);
        Permissions.AddAce("player.5", "command.kick", false);
        check.That("deny takes precedence over allow",
            !Permissions.IsAllowed("player.5", "command.kick"));
        check.That("the broader allow still covers a sibling object",
            Permissions.IsAllowed("player.5", "command.announce"));

        // Wildcards.
        Permissions.AddAce("player.6", "command.*", true);
        check.That("command.* covers command.kick", Permissions.IsAllowed("player.6", "command.kick"));
        check.That("command.* does not cover a different namespace",
            !Permissions.IsAllowed("player.6", "admin.shutdown"));
        Permissions.AddAce("player.6", "*", true);
        check.That("the catch-all covers anything", Permissions.IsAllowed("player.6", "admin.shutdown"));

        // --- group inheritance (group.mod, distinct from the seeded group.admin) ---
        Permissions.AddAce("group.mod", "command.*", true);
        Permissions.AddPrincipalToGroup("player.10", "group.mod");
        check.That("a player inherits its group's allow",
            Permissions.IsPlayerAllowed(10, "command.kick"));
        Permissions.RemovePrincipalFromGroup("player.10", "group.mod");
        check.That("removing membership revokes the inherited allow",
            !Permissions.IsPlayerAllowed(10, "command.kick"));

        // transitive inheritance: player.11 -> group.mod -> group.elders (which holds the ace)
        Permissions.AddAce("group.elders", "command.purge", true);
        Permissions.AddPrincipalToGroup("group.mod", "group.elders");
        Permissions.AddPrincipalToGroup("player.11", "group.mod");
        check.That("inheritance resolves transitively through nested groups",
            Permissions.IsPlayerAllowed(11, "command.purge"));

        // --- command path: permission gate on inbound admin:cmd ---
        bool handlerRan = false;
        Commands.Register("ping", "command.kick", _ => handlerRan = true);
        Permissions.AddAce("player.7", "command.kick", true);

        rec.Emits.Clear();
        Rpc.Dispatch("admin:cmd", 7u, false, new[] { Value.String("ping") });
        check.That("a permitted player's command runs", handlerRan);

        // a non-permitted player is denied: the handler does not run and a deny
        // reply is sent to just that sender.
        handlerRan = false;
        rec.Emits.Clear();
        Rpc.Dispatch("admin:cmd", 8u, false, new[] { Value.String("ping") });
        check.That("a denied player's command does not run", !handlerRan);
        check.That("the denied caller gets an Access denied reply",
            rec.Emits.Exists(e => e.Name == "admin:reply" && e.Target == RpcTarget.ToClient &&
                                  e.Peer == 8u && e.Args[0].AsString() == "Access denied"));

        // --- built-in hooks fire when their commands run ---
        uint kicked = 0;
        string announced = "";
        Admin.SetKickHandler(id => kicked = id);
        Admin.SetAnnounceHandler(text => announced = text);
        Permissions.AddAce("player.7", "command.announce", true);

        Rpc.Dispatch("admin:cmd", 7u, false, new[] { Value.String("kick"), Value.String("7") });
        check.Equal("the kick hook sees the parsed id", 7u, kicked);

        Rpc.Dispatch("admin:cmd", 7u, false,
            new[] { Value.String("announce"), Value.String("server"), Value.String("restarting") });
        check.Equal("the announce hook sees the whole message", "server restarting", announced);

        // setgroup mutates the permission graph through the command path.
        Permissions.AddAce("player.7", "command.setgroup", true);
        Rpc.Dispatch("admin:cmd", 7u, false,
            new[] { Value.String("setgroup"), Value.String("12"), Value.String("admin") });
        check.That("setgroup adds the target to the named group",
            Permissions.IsPlayerAllowed(12, "command.kick"));  // group.admin holds "*"

        // --- Reset clears permissions, commands and hooks ---
        Admin.Reset();
        check.That("reset clears permissions", !Permissions.IsAllowed("player.7", "command.kick"));

        // commands are gone: re-dispatching ping does nothing.
        handlerRan = false;
        Rpc.Dispatch("admin:cmd", 7u, false, new[] { Value.String("ping") });
        check.That("reset clears commands", !handlerRan);

        // hooks are back to no-op: re-bind (registers a fresh kick command), authorize
        // and run kick; the captured hook from before must not fire.
        kicked = 0;
        Admin.Bind(NetRole.Server);
        Permissions.AddAce("player.7", "command.kick", true);
        Rpc.Dispatch("admin:cmd", 7u, false, new[] { Value.String("kick"), Value.String("9") });
        check.Equal("reset cleared the kick hook", 0u, kicked);

        // --- standalone host is an implicit superadmin ---
        Admin.Reset();
        Platform.Reset();
        Platform.Boot(NetRole.Standalone);
        Admin.Bind(NetRole.Standalone);
        check.That("standalone local player is allowed everything",
            Permissions.IsPlayerAllowed(0, "command.anything"));

        Admin.Reset();
        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
    }
}
