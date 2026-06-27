using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Fallout carry-weight rule: crossing CarryWeight blocks fast travel and
// slows the player, the penalty applies once per transition (never stacking), and
// recovering below capacity restores both.
public static class FalloutEncumbranceTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        FastTravel.Clear();
        EventBus.Clear();

        fake.SetValue(fake.Player, ActorValue.CarryWeight, 100, 100);
        fake.SetValue(fake.Player, ActorValue.SpeedMult, 100, 100);
        fake.AddInventoryItem(fake.Player, item: 0x40, count: 6, weight: 10f);  // 60, under

        bool? lastOver = null;
        using var sub = EventBus.Subscribe<EncumbranceChanged>(e => lastOver = e.OverEncumbered);

        ModHost.Register(new Encumbrance { RecomputeInterval = 1.0f, SpeedPenalty = 40f });

        ModHost.Tick(1.0f);  // first recompute: 60 <= 100
        check.That("not over-encumbered under capacity", lastOver != true);
        check.That("fast travel enabled", fake.FastTravelEnabled);

        // Pile on loot: 60 + 60 = 120 > 100.
        fake.AddInventoryItem(fake.Player, item: 0x41, count: 6, weight: 10f);
        ModHost.Tick(1.0f);
        check.That("over-encumbered above capacity", lastOver == true);
        check.That("fast travel blocked", !fake.FastTravelEnabled);
        check.Equal("speed penalty applied", 60f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // A second recompute must not stack the penalty.
        ModHost.Tick(1.0f);
        check.Equal("penalty not stacked", 60f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // Raise capacity: recovers once.
        fake.SetValue(fake.Player, ActorValue.CarryWeight, 300, 300);
        ModHost.Tick(1.0f);
        check.That("recovered below capacity", lastOver == false);
        check.That("fast travel restored", fake.FastTravelEnabled);
        check.Equal("speed restored", 100f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        ModHost.Shutdown();
        FastTravel.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
