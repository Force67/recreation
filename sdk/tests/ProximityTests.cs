using System.Linq;
using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the proximity query: references within a radius are returned, the centre
// and far references are excluded.
public static class ProximityTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong center = 0x14;
        fake.SetPosition(center, 0, 0, 0);
        fake.SetPosition(0x100, 3, 0, 0);    // distance 3
        fake.SetPosition(0x101, 0, 4, 0);    // distance 4
        fake.SetPosition(0x102, 100, 0, 0);  // distance 100

        NearbyRef[] near = ObjectReference.From(center).RefsNear(5f);
        check.Equal("two references within radius", 2, near.Length);

        var handles = near.Select(n => n.Reference.Handle).ToHashSet();
        check.That("includes the references within range",
                   handles.Contains(0x100) && handles.Contains(0x101));
        check.That("excludes the far reference", !handles.Contains(0x102));
        check.That("excludes the centre", !handles.Contains(center));

        // Live distances are reported, so the nearest can be picked.
        NearbyRef nearest = near.OrderBy(n => n.Distance).First();
        check.Equal("nearest is the 3-away ref", 0x100UL, nearest.Reference.Handle);
        check.Equal("nearest distance is 3", 3f, nearest.Distance);

        // A reference not in the snapshot yields nothing.
        check.Equal("unknown centre yields none", 0, ObjectReference.From(0x999).RefsNear(5f).Length);

        Native.Backend = null;
    }
}
