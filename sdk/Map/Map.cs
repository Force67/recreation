using Recreation;

namespace Recreation.Net;

// The map subsystem's entry point. Wires the blip registry and owns the local user
// waypoint.
public static class Map
{
    // Bring the map up for a role: reset, then wire blip rendering. Call after
    // Platform.Boot (binds StateBags), since the blip observer rides StateBags.OnAnyChange.
    public static void Bind(NetRole role)
    {
        Reset();
        Blips.Bind(role);
    }

    // Tear the map down: clear blips, the waypoint and every subscription.
    public static void Reset()
    {
        Blips.Reset();
        global::Recreation.Net.Waypoint.Reset();
    }

    // The local user waypoint. Fully qualified because this property and the Waypoint
    // type share the name in this namespace.
    public static Vector3? Waypoint => global::Recreation.Net.Waypoint.Current;

    public static void SetWaypoint(Vector3 pos) => global::Recreation.Net.Waypoint.Set(pos);

    public static void ClearWaypoint() => global::Recreation.Net.Waypoint.Clear();
}
