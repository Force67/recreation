using System;
using System.Collections.Generic;

namespace Recreation.Modding;

// A spherical trigger volume around a reference, the managed analog of a Unity
// trigger collider or a gmod trigger brush. As references move within `Radius`
// (game units) of `Center`, Entered and Exited fire with the reference that
// crossed the boundary. Built on the proximity query and the frame loop, so it
// needs no engine support beyond the position snapshot.
//
//   var zone = Zone.Around(door, radius: 400f);
//   zone.Entered += who => Debug.Log($"{who} entered");
//   zone.Exited  += who => Debug.Log($"{who} left");
//
// A zone watches the same tracked references the proximity query sees (actors,
// triggers and spawned refs), not static clutter. Dispose to stop watching.
public sealed class Zone : IDisposable
{
    public ObjectReference Center { get; }
    public float Radius { get; set; }

    public event Action<ObjectReference>? Entered;
    public event Action<ObjectReference>? Exited;

    private readonly HashSet<ulong> _inside = new();

    private Zone(ObjectReference center, float radius)
    {
        Center = center;
        Radius = radius;
    }

    // Creates a zone around `center` and starts watching it on the frame loop.
    public static Zone Around(ObjectReference center, float radius)
    {
        var zone = new Zone(center, radius);
        Zones.Register(zone);
        return zone;
    }

    // The references currently inside the zone.
    public IReadOnlyCollection<ulong> Occupants => _inside;

    public bool Contains(ObjectReference reference) => _inside.Contains(reference.Handle);

    // Recomputes membership and fires the crossings. Called by Zones each scan.
    internal void Refresh()
    {
        var current = new HashSet<ulong>();
        foreach (NearbyRef near in Center.RefsNear(Radius)) current.Add(near.Reference.Handle);

        foreach (ulong handle in _inside)
            if (!current.Contains(handle)) Exited?.Invoke(ObjectReference.From(handle));
        foreach (ulong handle in current)
            if (!_inside.Contains(handle)) Entered?.Invoke(ObjectReference.From(handle));

        _inside.Clear();
        foreach (ulong handle in current) _inside.Add(handle);
    }

    // Stops watching the zone. Idempotent.
    public void Dispose() => Zones.Unregister(this);
}
