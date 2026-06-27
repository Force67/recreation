using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers exploration discovery: the first visit to a location pays XP and announces
// it, revisits and the open-world cell 0 do not, and enough discovery levels up.
public static class StarfieldDiscoveryTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        int discoveries = 0;
        using var sub = EventBus.Subscribe<LocationDiscovered>(_ => discoveries++);

        var discovery = new StarfieldDiscovery { XpPerDiscovery = 100f };
        ModHost.Register(discovery);

        const ulong lodge = 0x300, vault = 0x301, well = 0x302;

        EventBus.Publish(new LocationChanged(lodge, true));
        check.Equal("a discovery pays XP", 100f, CharacterProgress.Experience(player));
        check.Equal("one location discovered", 1, discovery.DiscoveredCount);
        check.Equal("a discovery event fired", 1, discoveries);

        // Revisiting pays nothing.
        EventBus.Publish(new LocationChanged(lodge, true));
        check.Equal("revisits do not pay", 100f, CharacterProgress.Experience(player));
        check.Equal("still one discovered", 1, discovery.DiscoveredCount);

        // Stepping outside (cell 0) is not a discovery.
        EventBus.Publish(new LocationChanged(0, false));
        check.Equal("the open world is not a discovery", 1, discovery.DiscoveredCount);

        // A second new location tips the character to level 2 (175 needed).
        EventBus.Publish(new LocationChanged(vault, true));
        check.Equal("reached level 2 from exploration", 2, CharacterProgress.Level(player));
        check.Equal("two locations discovered", 2, discovery.DiscoveredCount);

        // Once removed, discoveries stop paying.
        ModHost.Unregister(discovery);
        EventBus.Publish(new LocationChanged(well, true));
        check.Equal("two discovery events total", 2, discoveries);

        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
