using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Starfield mass limit: crossing capacity blocks fast travel, slows the
// player and flags the O2 loop as overburdened, and recovering restores all three.
public static class StarfieldMassTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        fake.SetValue(fake.Player, ActorValue.CarryWeight, 100, 100);
        fake.SetValue(fake.Player, ActorValue.SpeedMult, 100, 100);
        fake.AddInventoryItem(fake.Player, item: 0x40, count: 6, weight: 10f);  // 60 mass, under

        bool? lastOver = null;
        using var sub = EventBus.Subscribe<OverMassChanged>(e => lastOver = e.OverMass);

        // The O2 system must be registered first so the mass system can find it.
        var oxygen = new OxygenCo2();
        ModHost.Register(oxygen);
        ModHost.Register(new MassEncumbrance { RecomputeInterval = 1.0f, SpeedPenalty = 35f });

        ModHost.Tick(1.0f);  // first recompute: 60 <= 100, no change
        check.That("not over-mass under capacity", lastOver != true);
        check.That("fast travel enabled", fake.FastTravelEnabled);
        check.That("oxygen not overburdened", !oxygen.Overburdened);

        // Pile on cargo: 60 + 60 = 120 > 100.
        fake.AddInventoryItem(fake.Player, item: 0x41, count: 6, weight: 10f);
        ModHost.Tick(1.0f);
        check.That("over-mass above capacity", lastOver == true);
        check.That("fast travel blocked", !fake.FastTravelEnabled);
        check.Equal("speed penalty applied", 65f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));
        check.That("oxygen flagged overburdened", oxygen.Overburdened);

        // Overburden now drains oxygen with no sprint input.
        float before = oxygen.Oxygen;
        ModHost.Tick(1.0f);
        check.That("overburden drains oxygen", oxygen.Oxygen < before);

        // A second recompute must not stack the penalty.
        ModHost.Tick(1.0f);
        check.Equal("penalty not stacked", 65f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));

        // Raise capacity: recovers once and clears the overburden flag.
        fake.SetValue(fake.Player, ActorValue.CarryWeight, 300, 300);
        ModHost.Tick(1.0f);
        check.That("recovered below capacity", lastOver == false);
        check.That("fast travel restored", fake.FastTravelEnabled);
        check.Equal("speed restored", 100f, fake.GetCurrent(fake.Player, ActorValue.SpeedMult));
        check.That("overburden cleared", !oxygen.Overburdened);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
