using System.Runtime.InteropServices;

namespace Recreation.Interop;

// Byte-for-byte mirror of host/bridge.h ScriptBridge: the table of function
// pointers the engine hands the managed host. Same field order, same calling
// convention (unmanaged, the platform default the engine's C++ uses). Append
// only, never reorder.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ScriptBridge
{
    public void* Ctx;

    public delegate* unmanaged<void*, byte*, int> IsScriptLoaded;
    public delegate* unmanaged<void*, byte*, int> LoadScript;
    public delegate* unmanaged<void*, byte*, ulong> CreateInstance;
    public delegate* unmanaged<void*, ulong, byte*, int, int> TypeOf;

    public delegate* unmanaged<void*, byte*, byte*, ApiValue*, int, ApiValue*, void> CallGlobal;
    public delegate* unmanaged<void*, ulong, byte*, ApiValue*, int, ApiValue*, void> CallMethod;

    public delegate* unmanaged<void*, ulong, byte*, ApiValue*, void> GetProperty;
    public delegate* unmanaged<void*, ulong, byte*, ApiValue, void> SetProperty;

    public delegate* unmanaged<void*, float, void> Tick;
}
