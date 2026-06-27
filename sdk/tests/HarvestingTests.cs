using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers harvesting: activating a producing flora adds its ingredient and picks
// the plant, while non-flora and barren flora are left alone.
public static class HarvestingTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong plant = 0x800;       // the placed flora reference
        const ulong floraBase = 0x900;   // its FLOR base
        const ulong ingredient = 0xA00;  // the produce
        fake.SetBase(plant, floraBase, essential: false);
        fake.SetFormType(floraBase, 40);  // FLOR
        fake.SetHarvestIngredient(floraBase, ingredient);

        ulong harvested = 0;
        using var sub = EventBus.Subscribe<FloraHarvested>(e => harvested = e.IngredientHandle);

        ModHost.Register(new Harvesting());

        EventBus.Publish(new PlayerActivated(plant));
        check.Equal("ingredient added to the player", 1,
                    ObjectReference.From(fake.Player).GetItemCount(Form.From(ingredient)));
        check.That("plant is picked (disabled)", ObjectReference.From(plant).IsDisabled);
        check.Equal("harvest event raised", ingredient, harvested);

        // Activating a non-flora does nothing.
        const ulong door = 0x801;
        fake.SetBase(door, 0x901, essential: false);
        fake.SetFormType(0x901, 30);  // DOOR
        harvested = 0;
        EventBus.Publish(new PlayerActivated(door));
        check.Equal("non-flora is not harvested", 0UL, harvested);

        // A barren flora (no produce) is left alone.
        const ulong stump = 0x802;
        fake.SetBase(stump, 0x902, essential: false);
        fake.SetFormType(0x902, 40);  // FLOR, but no ingredient set
        EventBus.Publish(new PlayerActivated(stump));
        check.That("barren flora is not picked", !ObjectReference.From(stump).IsDisabled);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
