using Recreation.Games.Skyrim;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers first-visit discovery: each new interior is announced once, revisits
// are silent, and exteriors are ignored.
public static class LocationDiscoveryTests
{
    public static void Run(Check check)
    {
        ModHost.Shutdown();
        EventBus.Clear();

        ulong lastDiscovered = 0;
        int discoveries = 0;
        using var sub = EventBus.Subscribe<LocationDiscovered>(e =>
        {
            lastDiscovered = e.CellHandle;
            discoveries++;
        });

        var discovery = new LocationDiscovery();
        ModHost.Register(discovery);

        EventBus.Publish(new LocationChanged(0x300, isInterior: true));
        check.Equal("first entry is a discovery", 1, discoveries);
        check.Equal("the right cell", 0x300UL, lastDiscovered);
        check.Equal("counted as visited", 1, discovery.VisitedCount);

        // Revisiting the same cell is not a new discovery.
        EventBus.Publish(new LocationChanged(0, isInterior: false));   // step outside
        EventBus.Publish(new LocationChanged(0x300, isInterior: true));  // back in
        check.Equal("revisit is silent", 1, discoveries);

        // A different interior is a new discovery.
        EventBus.Publish(new LocationChanged(0x301, isInterior: true));
        check.Equal("new location discovered", 2, discoveries);
        check.Equal("two locations visited", 2, discovery.VisitedCount);

        // Exteriors are not tracked as discoveries.
        EventBus.Publish(new LocationChanged(0, isInterior: false));
        check.Equal("exterior is not a discovery", 2, discoveries);

        ModHost.Shutdown();
        EventBus.Clear();
    }
}
