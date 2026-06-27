using System;
using Recreation;
using Recreation.Interop;

namespace Recreation.Net;

// The local player's user waypoint. Always local, so it needs no state bag, just a
// held position and its own HUD channel.
public static class Waypoint
{
    private static Vector3? _position;

    // The current waypoint, or null when none is set.
    public static Vector3? Current => _position;

    // Drop (or move) the waypoint and draw it.
    public static void Set(Vector3 pos)
    {
        _position = pos;
        CallHud("Waypoint", Value.Float(pos.X), Value.Float(pos.Y), Value.Float(pos.Z));
    }

    // Remove the waypoint. A no-op (and no redundant HUD call) when none is set.
    public static void Clear()
    {
        if (_position == null) return;
        _position = null;
        CallHud("ClearWaypoint");
    }

    // Drop the waypoint on session teardown.
    internal static void Reset() => Clear();

    // Route to the engine HUD. Returns None when the HUD is not wired, so this is
    // safe anywhere.
    private static void CallHud(string function, params ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Hud", function, args);
}
