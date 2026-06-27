using Recreation.Modding;

namespace Recreation.Tests;

// Fixtures: one mod per realm, plus a mod with no [Realm] (defaults to Server).
// Top-level public with an empty OnLoad so ModDiscovery sees them and they are
// harmless if discovered by another path.
[Realm(ModRealm.Server)] public sealed class RealmServerMod : IMod { public void OnLoad() { } }
[Realm(ModRealm.Client)] public sealed class RealmClientMod : IMod { public void OnLoad() { } }
[Realm(ModRealm.Shared)] public sealed class RealmSharedMod : IMod { public void OnLoad() { } }
public sealed class RealmDefaultMod : IMod { public void OnLoad() { } }

// Verifies the realm filter: a server runs Server + Shared (and untagged) mods, a
// client runs Client + Shared, single-player runs everything. This is what keeps
// authoritative gameplay mods off connecting clients.
public static class RealmTests
{
    public static void Run(Check check)
    {
        var asm = new[] { typeof(RealmServerMod).Assembly };

        var server = ModDiscovery.FindMods(asm, ModDiscovery.HostServer);
        check.That("server runs the Server mod", server.Contains(typeof(RealmServerMod)));
        check.That("server runs the Shared mod", server.Contains(typeof(RealmSharedMod)));
        check.That("server runs an untagged (Server) mod", server.Contains(typeof(RealmDefaultMod)));
        check.That("server skips the Client mod", !server.Contains(typeof(RealmClientMod)));

        var client = ModDiscovery.FindMods(asm, ModDiscovery.HostClient);
        check.That("client runs the Client mod", client.Contains(typeof(RealmClientMod)));
        check.That("client runs the Shared mod", client.Contains(typeof(RealmSharedMod)));
        check.That("client skips the Server mod", !client.Contains(typeof(RealmServerMod)));
        check.That("client skips an untagged (Server) mod", !client.Contains(typeof(RealmDefaultMod)));

        var standalone = ModDiscovery.FindMods(asm, ModDiscovery.HostStandalone);
        check.That("standalone runs the Server mod", standalone.Contains(typeof(RealmServerMod)));
        check.That("standalone runs the Client mod", standalone.Contains(typeof(RealmClientMod)));
        check.That("standalone runs the Shared mod", standalone.Contains(typeof(RealmSharedMod)));

        check.Equal("untagged realm is Server", ModRealm.Server, ModDiscovery.RealmOf(typeof(RealmDefaultMod)));
        check.Equal("tagged realm is read", ModRealm.Client, ModDiscovery.RealmOf(typeof(RealmClientMod)));
    }
}
