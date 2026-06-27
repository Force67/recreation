using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the carry-weight rule: crossing capacity disables fast travel and slows
// the player, and recovering restores both, exactly once each.
public static class EncumbranceTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        // Capacity 100, normal speed.
        fake.SetValue(fake.Player, ActorValue.CarryWeight, 100, 100);
        fake.SetValue(fake.Player, ActorValue.SpeedMult, 100, 100);
        // 60 weight of goods: under capacity.
        fake.AddInventoryItem(fake.Player, item: 0x10, count: 6, weight: 10f);

        bool? lastOver = null;
        using var sub = EventBus.Subscribe<EncumbranceChanged>(e => lastOver = e.OverEncumbered);

        ModHost.Register(new Encumbrance { RecomputeInterval = 1.0f, SpeedPenalty = 40f });

        ModHost.Tick(1.0f);  // first recompute: 60 <= 100, no change
        check.That("not over-encumbered under capacity", lastOver != true);
        check.That("fast travel stays enabled", fake.FastTravelEnabled);
        check.Equal("no speed penalty", 100f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // Pile on weight: 6 + 6 = 12 items * 10 = 120 > 100.
        fake.AddInventoryItem(fake.Player, item: 0x11, count: 6, weight: 10f);
        ModHost.Tick(1.0f);
        check.That("over-encumbered when above capacity", lastOver == true);
        check.That("fast travel disabled", !fake.FastTravelEnabled);
        check.Equal("speed penalty applied", 60f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // A second recompute must not stack the penalty.
        ModHost.Tick(1.0f);
        check.Equal("penalty not stacked", 60f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // Raise capacity (as if a buff): now under, recovers once.
        fake.SetValue(fake.Player, ActorValue.CarryWeight, 200, 200);
        ModHost.Tick(1.0f);
        check.That("recovered below capacity", lastOver == false);
        check.That("fast travel restored", fake.FastTravelEnabled);
        check.Equal("speed restored", 100f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // Responsiveness: an inventory change recomputes within a frame, not only
        // on the interval. Drop capacity below the load and publish the event.
        fake.SetValue(fake.Player, ActorValue.CarryWeight, 50, 50);
        EventBus.Publish(new ItemAdded(fake.Player, 0x12, 1));
        ModHost.Tick(0.1f);  // far less than the 1s interval
        check.That("inventory change recomputes immediately", !fake.FastTravelEnabled);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
