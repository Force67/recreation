using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers trigger zones: references crossing the radius fire Entered/Exited, and
// membership tracks as they move in and out.
public static class ZoneTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();
        Zones.ScanInterval = 0f;  // scan every frame for a deterministic test

        const ulong center = 0x14, actorA = 0x100, actorB = 0x101;
        fake.SetPosition(center, 0, 0, 0);
        fake.SetPosition(actorA, 3, 0, 0);    // within radius 5
        fake.SetPosition(actorB, 100, 0, 0);  // far outside

        var entered = new List<ulong>();
        var exited = new List<ulong>();
        Zone zone = Zone.Around(ObjectReference.From(center), 5f);
        zone.Entered += r => entered.Add(r.Handle);
        zone.Exited += r => exited.Add(r.Handle);

        // First scan: A is inside, B is not.
        ModHost.Tick(0.1f);
        check.That("nearby reference entered", entered.Contains(actorA));
        check.That("far reference did not enter", !entered.Contains(actorB));
        check.That("zone reports the occupant", zone.Contains(ObjectReference.From(actorA)));
        check.Equal("one occupant", 1, zone.Occupants.Count);

        // A stays inside: no repeat enter.
        entered.Clear();
        ModHost.Tick(0.1f);
        check.Equal("no repeat enter while inside", 0, entered.Count);

        // A leaves the radius: Exited fires.
        fake.SetPosition(actorA, 100, 0, 0);
        ModHost.Tick(0.1f);
        check.That("reference that left fired Exited", exited.Contains(actorA));
        check.Equal("zone is empty", 0, zone.Occupants.Count);

        // B moves in: Entered fires for it.
        entered.Clear();
        fake.SetPosition(actorB, 2, 0, 0);
        ModHost.Tick(0.1f);
        check.That("reference that arrived fired Entered", entered.Contains(actorB));

        // Disposing stops tracking.
        zone.Dispose();
        check.Equal("disposed zone is unregistered", 0, Zones.ActiveCount);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
