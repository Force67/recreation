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
        if (Environment.GetEnvironmentVariable("RX_DOMAINS_REPORT") != null) ReportDomains();
        // Bring in the optional default gamemodes (the per-game rulesets) as
        // separate assemblies before the boot scan, so they load like the built-ins
        // they replaced. Absent (or disabled) just means a barebones session.
        ModLoader.PreloadDefaultGamemodes();
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

    // RX_DOMAINS_REPORT: list every game a mod can reach this session.
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

    // Reports the effective GC configuration back to the native gcprofiletest
    // harness, so a test can confirm the per-platform runtime properties ClrHost
    // set actually took effect (a heap hard limit shows up as a bounded
    // TotalAvailableMemoryBytes; server GC flips IsServerGC).
    [StructLayout(LayoutKind.Sequential)]
    private struct GcInfo
    {
        public long TotalAvailableBytes;  // GC's memory ceiling (the heap hard limit, if set)
        public long HeapSizeBytes;        // currently allocated managed heap
        public int IsServerGc;            // 0 workstation, 1 server
        public int LatencyMode;           // System.Runtime.GCLatencyMode
    }

    [UnmanagedCallersOnly]
    public static int GcReport(void* infoPtr)
    {
        var g = (GcInfo*)infoPtr;
        GCMemoryInfo info = GC.GetGCMemoryInfo();
        g->TotalAvailableBytes = info.TotalAvailableMemoryBytes;
        g->HeapSizeBytes = GC.GetTotalMemory(forceFullCollection: false);
        g->IsServerGc = System.Runtime.GCSettings.IsServerGC ? 1 : 0;
        g->LatencyMode = (int)System.Runtime.GCSettings.LatencyMode;
        return 0;
    }

    // Correctness check for the engine boundary, driven by the native bridgetest
    // harness both cross-thread (host thread -> guest hop) and on the guest thread
    // (Dispatch fast path). Runs a battery of calls through NativeBackend and
    // returns the number of failed checks (0 = all passed). Exercises the arena
    // marshalling: no-arg calls, integer args, single and multiple string args,
    // unicode, an empty string, a string larger than the arena's initial block
    // (forcing growth), and a reuse loop. Both invocations must return the same
    // correct results, which is what proves the fast path did not change behaviour.
    [UnmanagedCallersOnly]
    public static int BridgeCheck(void* bridge)
    {
        var backend = new NativeBackend((ScriptBridge*)bridge);
        int failures = 0;
        void Check(bool ok, string what)
        {
            if (ok) return;
            failures++;
            Console.Error.WriteLine($"[bridgecheck] FAIL: {what}");
        }

        Check(backend.CallGlobal("Game", "GetPlayer", default).AsHandle() == 0x14, "GetPlayer == 0x14");
        Check(backend.CallGlobal("Bench", "Echo", new[] { Value.String("Health") }).AsString() == "Health",
              "Echo(\"Health\")");
        const string unicode = "naïve—Скайрим—日本語";
        Check(backend.CallGlobal("Bench", "Echo", new[] { Value.String(unicode) }).AsString() == unicode,
              "Echo(unicode)");
        Check(backend.CallGlobal("Bench", "AddInts", new[] { Value.Int(3), Value.Int(4) }).AsInt() == 7,
              "AddInts(3,4) == 7");
        Check(backend.CallGlobal("Bench", "Join",
                  new[] { Value.String("a"), Value.String("b"), Value.String("c") }).AsString() == "abc",
              "Join(a,b,c) == abc");
        string big = new string('x', 4096);  // larger than the arena's initial block
        Check(backend.CallGlobal("Bench", "Echo", new[] { Value.String(big) }).AsString() == big,
              "Echo(4096 chars)");
        Check(backend.CallGlobal("Bench", "Echo", new[] { Value.String(string.Empty) }).AsString() == string.Empty,
              "Echo(empty)");

        // Repeated calls must reuse the same arena without corrupting results.
        bool reuseOk = true;
        for (int i = 0; i < 2000 && reuseOk; i++)
            reuseOk = backend.CallGlobal("Bench", "Echo", new[] { Value.String("Health") }).AsString() == "Health";
        Check(reuseOk, "2000x Echo reuse");

        return failures;
    }

    // Throughput benchmark. Binds the given bridge and runs a fixed sequence of
    // engine calls `Iters` times, recording the managed bytes allocated over the
    // loop into the args struct. The native benchbridge harness invokes this both
    // cross-thread (from the host thread, so each call round-trips to the guest)
    // and on the guest thread (so Dispatch takes the direct path), and times each.
    // Purely a measurement entrypoint; the production boot never touches it.
    [StructLayout(LayoutKind.Sequential)]
    private struct BenchArgs
    {
        public ScriptBridge* Bridge;
        public long Iters;
        public long AllocBytes;  // out: managed bytes allocated during the timed loop
    }

    [UnmanagedCallersOnly]
    public static int Bench(void* argsPtr)
    {
        var a = (BenchArgs*)argsPtr;
        var backend = new NativeBackend(a->Bridge);
        // Allocated once, before the measured loop, so it never counts toward the
        // per-call allocation figure (Value holds a string, so it cannot be stack
        // allocated anyway).
        var echoArg = new Value[] { Value.String("Health") };

        // Warm up: let the JIT tier up the call path and take any one-time hits
        // before the measured loop, so the numbers reflect steady state.
        for (int i = 0; i < 2000; i++)
        {
            backend.CallGlobal("Game", "GetPlayer", default);
            backend.CallGlobal("Bench", "Echo", echoArg);
        }

        long before = GC.GetAllocatedBytesForCurrentThread();
        for (long i = 0; i < a->Iters; i++)
        {
            backend.CallGlobal("Game", "GetPlayer", default);
            backend.CallGlobal("Bench", "Echo", echoArg);
        }
        a->AllocBytes = GC.GetAllocatedBytesForCurrentThread() - before;
        return 0;
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
