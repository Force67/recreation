using System;
using System.Runtime.InteropServices;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation;

// The native entry into the managed world. The engine's ManagedHost resolves one
// of these [UnmanagedCallersOnly] methods and calls it across the boundary. Both
// first bind the bridge so the rest of the SDK (Game, Form, mods, ...) can reach
// the engine.
public static unsafe class ScriptHost
{
    // Production boot. Binds the inbound bridge, starts the mod host, then hands
    // the engine the outbound callbacks it drives the managed world through.
    // Returns 0 on success.
    [UnmanagedCallersOnly]
    public static int Main(void* handshakePtr)
    {
        var handshake = (HostHandshake*)handshakePtr;
        Native.Bind(handshake->Bridge);
        RegisterDomains(handshake);
        // Wire the ultragui UI layer if a windowed client gave us its widget ops,
        // so [UiHandler] methods drive ultragui in place of Lua. Absent (null) on
        // the dedicated server; Ui then stays an inert no-op.
        if (handshake->UiWidgetOps != null) Ui.Bind(handshake->UiWidgetOps);
        // Wire the multiplayer RPC layer. Emit is null in single-player, leaving
        // Rpc unbound so sends and dispatch are inert.
        if (handshake->Rpc.Emit != null) Rpc.Bind(new NativeRpcBackend(handshake->Rpc));
        // Bring the multiplayer platform (state bags, player registry, ...) online
        // for this process role, before mods load so an OnLoad can use it.
        Recreation.Net.Platform.Boot(handshake->Realm);
        Console.WriteLine($"[managed] Recreation scripting host online (SDK {SdkInfo.Version}), {Domains.Count} game(s)");
        if (Environment.GetEnvironmentVariable("REC_DOMAINS_REPORT") != null) ReportDomains();
        // Filter which mods start by the process role: server + shared on a host,
        // client + shared on a client, everything in single-player.
        ModHost.SetHostRealm(handshake->Realm);
        ModHost.Boot();

        // Load drop-in user mods from RECREATION_MODS_DIR, if set.
        string? modsDir = Environment.GetEnvironmentVariable("RECREATION_MODS_DIR");
        if (!string.IsNullOrEmpty(modsDir)) ModLoader.LoadDirectory(modsDir);

        handshake->Callbacks.Tick = &OnTick;
        handshake->Callbacks.PublishEvent = &OnPublishEvent;
        handshake->Callbacks.Shutdown = &OnShutdown;
        handshake->Callbacks.DispatchUi = &OnDispatchUi;
        handshake->Callbacks.DispatchRpc = &OnDispatchRpc;
        return 0;
    }

    // Builds the Domains registry from the handshake's per-game bridge table, so
    // mods can reach every loaded game. The primary (rendered) game is entry 0
    // and reuses the bound Native backend; the rest each get their own backend.
    private static void RegisterDomains(HostHandshake* handshake)
    {
        Domains.Clear();
        for (int i = 0; i < handshake->DomainCount; i++)
        {
            DomainBridge db = handshake->Domains[i];
            string name = Marshal.PtrToStringUTF8((IntPtr)db.Name) ?? $"domain{i}";
            IEngineBackend backend =
                i == 0 ? Native.Backend! : new NativeBackend(db.Bridge);
            Domains.Register(new GameWorld(name, backend), isPrimary: i == 0);
        }
        // The bare SelfTest path passes no domain table; fall back to one world
        // over the bound primary backend so Domains is never empty when bound.
        if (Domains.Count == 0 && Native.Backend != null)
            Domains.Register(new GameWorld("primary", Native.Backend), isPrimary: true);
    }

    // REC_DOMAINS_REPORT: list every game a mod can reach this session.
    private static void ReportDomains()
    {
        Console.WriteLine($"[managed] {Domains.Count} game domain(s) live:");
        foreach (GameWorld world in Domains.All)
            Console.WriteLine($"[managed]   - {world.Name}{(world == Domains.Primary ? " (primary)" : "")}");
    }

    // Self-test: binds the bridge and exercises the SDK against the engine,
    // returning the number of failed checks (0 = all passed). The native
    // hosttest harness asserts on this. Separate from Main so the production
    // boot never assumes a test fixture.
    [UnmanagedCallersOnly]
    public static int SelfTest(void* bridge)
    {
        Native.Bind((ScriptBridge*)bridge);
        return SdkSelfTest.Run();
    }

    [UnmanagedCallersOnly]
    private static void OnTick(float deltaTime)
    {
        ModHost.Tick(deltaTime);
        Ui.Tick(deltaTime);  // advance UI timers (Ui.After)
    }

    // A ultragui handler fired; route it to the managed UI layer. Returns 1 if a
    // [UiHandler] claimed it.
    [UnmanagedCallersOnly]
    private static int OnDispatchUi(byte* funcName, ulong widget)
    {
        string name = Marshal.PtrToStringUTF8((IntPtr)funcName) ?? string.Empty;
        return Ui.Dispatch(name, widget);
    }

    // An inbound multiplayer RPC arrived; unpack the wire args and route it to the
    // managed Rpc layer, where the registered handler (if any) runs.
    [UnmanagedCallersOnly]
    private static void OnDispatchRpc(byte* name, int sender, int fromServer, ApiValue* args, int argc)
    {
        string n = Marshal.PtrToStringUTF8((IntPtr)name) ?? string.Empty;
        var values = new Value[argc < 0 ? 0 : argc];
        for (int i = 0; i < values.Length; i++) values[i] = FromApi(args[i]);
        Rpc.Dispatch(n, (uint)sender, fromServer != 0, values);
    }

    // Wire ApiValue -> Value, matching NativeBackend.FromApi (private there).
    private static Value FromApi(in ApiValue v) => v.Kind switch
    {
        ApiKind.Int => Value.Int(v.I),
        ApiKind.Float => Value.Float(v.F),
        ApiKind.Bool => Value.Bool(v.I != 0),
        ApiKind.String => Value.String(v.S != null ? Marshal.PtrToStringUTF8((IntPtr)v.S) ?? "" : ""),
        ApiKind.Object => Value.Object(v.H),
        ApiKind.Array => Value.Array(v.H),
        _ => Value.None,
    };

    [UnmanagedCallersOnly]
    private static void OnPublishEvent(ManagedEvent* e) => EngineEvents.Dispatch(*e);

    [UnmanagedCallersOnly]
    private static void OnShutdown() => ModHost.Shutdown();
}
