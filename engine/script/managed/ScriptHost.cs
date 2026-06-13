using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Recreation;

// Mirrors engine/script/host/bridge.h GuestBridge exactly: same field order,
// same calling convention (the platform default, matching the engine's C++
// functions). Append-only, never reorder.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct GuestBridge
{
    public void* Ctx;
    public delegate* unmanaged<void*, byte*, int> LoadScript;
    public delegate* unmanaged<void*, byte*, ulong> CreateInstance;
    public delegate* unmanaged<void*, ulong, byte*, void> RaiseEvent;
    public delegate* unmanaged<void*, ulong, byte*, int> GetIntProperty;
    public delegate* unmanaged<void*, ulong, byte*, int, void> SetIntProperty;
    public delegate* unmanaged<void*, float, void> Tick;
}

// The ergonomic, managed-side handle to the Papyrus guest. This is the surface
// user C# calls: the guest is another world reached only through these methods,
// never a shared object graph.
public readonly unsafe struct PapyrusGuest
{
    private readonly GuestBridge* _bridge;

    internal PapyrusGuest(GuestBridge* bridge) => _bridge = bridge;

    public bool IsScriptLoaded(string type)
    {
        using var utf8 = new Utf8(type);
        return _bridge->LoadScript(_bridge->Ctx, utf8.Ptr) != 0;
    }

    public ulong CreateInstance(string type)
    {
        using var utf8 = new Utf8(type);
        return _bridge->CreateInstance(_bridge->Ctx, utf8.Ptr);
    }

    public void RaiseEvent(ulong instance, string eventName)
    {
        using var utf8 = new Utf8(eventName);
        _bridge->RaiseEvent(_bridge->Ctx, instance, utf8.Ptr);
    }

    public int GetInt(ulong instance, string property)
    {
        using var utf8 = new Utf8(property);
        return _bridge->GetIntProperty(_bridge->Ctx, instance, utf8.Ptr);
    }

    public void SetInt(ulong instance, string property, int value)
    {
        using var utf8 = new Utf8(property);
        _bridge->SetIntProperty(_bridge->Ctx, instance, utf8.Ptr, value);
    }

    public void Tick(float seconds) => _bridge->Tick(_bridge->Ctx, seconds);

    // RAII UTF-8 marshalling for the short-lived string arguments above.
    private readonly unsafe struct Utf8 : IDisposable
    {
        private readonly IntPtr _ptr;
        public Utf8(string s) => _ptr = Marshal.StringToCoTaskMemUTF8(s);
        public byte* Ptr => (byte*)_ptr;
        public void Dispose() => Marshal.FreeCoTaskMem(_ptr);
    }
}

// The managed entrypoint the engine's ClrHost resolves and calls. This is where
// the two worlds meet: the host (this .NET code) drives the Papyrus guest.
public static unsafe class ScriptHost
{
    [UnmanagedCallersOnly]
    public static int Main(void* bridge)
    {
        var guest = new PapyrusGuest((GuestBridge*)bridge);
        return Run(guest);
    }

    // User-facing logic. Demonstrates driving a guest script from C#: create an
    // instance, kick off its update loop, advance time, read a property back.
    private static int Run(PapyrusGuest guest)
    {
        Console.WriteLine("[managed] .NET host running, calling into the Papyrus guest");

        if (!guest.IsScriptLoaded("Ticker"))
        {
            Console.WriteLine("[managed] Ticker script is not loaded");
            return -1;
        }

        ulong ticker = guest.CreateInstance("Ticker");
        Console.WriteLine($"[managed] created Ticker instance handle={ticker}");
        if (ticker == 0)
            return -1;

        guest.RaiseEvent(ticker, "begin");  // schedules a guest update
        guest.Tick(0.6f);                    // advance past the 0.5s timer

        int count = guest.GetInt(ticker, "Count");
        Console.WriteLine($"[managed] Ticker.Count after one update = {count}");
        return count;
    }
}
