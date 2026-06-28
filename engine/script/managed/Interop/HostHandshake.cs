using System.Runtime.InteropServices;

namespace Recreation.Interop;

// The id of an engine event crossing into managed code. Mirrors host/bridge.h
// ManagedEventId; append only, never reorder.
public enum ManagedEventId : int
{
    None = 0,
    ActorDied = 1,           // A = actor handle
    ItemAdded = 2,           // A = container handle, B = item handle, I = count
    QuestStageChanged = 3,   // A = quest form id, I = stage
    FormLoaded = 4,          // A = form handle (its scripts attached / it went live)
    PlayerActivated = 5,     // A = activated target handle
}

// An engine event payload. Byte-for-byte mirror of host/bridge.h ManagedEvent;
// only the fields an id documents are meaningful.
[StructLayout(LayoutKind.Sequential)]
public struct ManagedEvent
{
    public ManagedEventId Id;
    public ulong A;
    public ulong B;
    public int I;
    public float F;
}

// The outbound callbacks the managed entrypoint fills so the engine can drive
// the managed world. Mirror of host/bridge.h HostCallbacks.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct HostCallbacks
{
    public delegate* unmanaged<float, void> Tick;
    public delegate* unmanaged<ManagedEvent*, void> PublishEvent;
    public delegate* unmanaged<void> Shutdown;
}

// What the managed entrypoint receives: the inbound bridge plus the outbound
// callbacks slot to fill. Mirror of host/bridge.h HostHandshake.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct HostHandshake
{
    public ScriptBridge* Bridge;
    public HostCallbacks Callbacks;
}
