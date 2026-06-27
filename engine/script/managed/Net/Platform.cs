namespace Recreation.Net;

// The single entry point that brings the multiplayer platform online and tears it
// down. The script host boots it once the RPC channel and host realm are known
// (before mods load, so a mod's OnLoad can already touch platform state); the mod
// host resets it on shutdown so a session reload starts clean.
//
// Every platform subsystem (state bags today; player registry, chat, social and
// the rest as they land) is wired from here, so the host integration stays a
// single call on each side no matter how the platform grows.
public static class Platform
{
    public static NetRole Role { get; private set; } = NetRole.Standalone;

    // True where this process is authoritative: the host, or single-player. The
    // everyday guard for "should I run authoritative logic here".
    public static bool IsServer => Role != NetRole.Client;
    public static bool IsClient => Role == NetRole.Client;
    public static bool IsStandalone => Role == NetRole.Standalone;
    public static bool IsNetworked => Role != NetRole.Standalone;

    // Bring the platform up for a host realm (the raw int from the handshake:
    // 0 server, 1 client, anything else standalone). Called by ScriptHost.Main.
    internal static void Boot(int realm) => Boot(RoleFromRealm(realm));

    // Bring the platform up for a known role. Public so tests (and tooling) can
    // drive a specific side without a handshake.
    public static void Boot(NetRole role)
    {
        Role = role;
        // Foundation first (state + roster), then the systems that build on them.
        StateBags.Bind(role);
        Players.Bind(role);
        Chat.Bind(role);
        Social.Bind(role);
        Admin.Bind(role);
        Persistence.Bind(role);
        // Client-facing UI surfaces (no-op-safe rendering until the HUD is wired).
        HudKit.Bind(role);
        Map.Bind(role);
        Scoreboard.Bind(role);
        ServerBrowser.Bind(role);
        NetEntities.Bind(role);
        // Roleplay primitives layered on the foundation.
        Teams.Bind(role);
        Economy.Bind(role);
        Voice.Bind(role);
    }

    // Tear every subsystem down, in reverse of Boot. Called by ModHost.Shutdown.
    public static void Reset()
    {
        Voice.Reset();
        Economy.Reset();
        Teams.Reset();
        NetEntities.Reset();
        ServerBrowser.Reset();
        Scoreboard.Reset();
        Map.Reset();
        HudKit.Reset();
        Persistence.Reset();
        Admin.Reset();
        Social.Reset();
        Chat.Reset();
        Players.Reset();
        StateBags.Reset();
        Role = NetRole.Standalone;
    }

    public static NetRole RoleFromRealm(int realm) => realm switch
    {
        0 => NetRole.Server,
        1 => NetRole.Client,
        _ => NetRole.Standalone,
    };
}
