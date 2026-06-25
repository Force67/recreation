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
        Console.WriteLine("[managed] Recreation scripting host online");
        ModHost.Boot();

        // Load drop-in user mods from RECREATION_MODS_DIR, if set.
        string? modsDir = Environment.GetEnvironmentVariable("RECREATION_MODS_DIR");
        if (!string.IsNullOrEmpty(modsDir)) ModLoader.LoadDirectory(modsDir);

        handshake->Callbacks.Tick = &OnTick;
        handshake->Callbacks.PublishEvent = &OnPublishEvent;
        handshake->Callbacks.Shutdown = &OnShutdown;
        return 0;
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
    private static void OnTick(float deltaTime) => ModHost.Tick(deltaTime);

    [UnmanagedCallersOnly]
    private static void OnPublishEvent(ManagedEvent* e) => EngineEvents.Dispatch(*e);

    [UnmanagedCallersOnly]
    private static void OnShutdown() => ModHost.Shutdown();
}
