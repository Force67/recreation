using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Starfield ship layer: a grav jump spends fuel and is refused beyond the
// drive's range or beyond the fuel, refueling tops the tank without overflowing, an
// over-mass hold grounds the drive and blocks fast travel until trimmed, and the
// reachable range reflects both fuel and the capacity gate.
public static class StarfieldShipSystemsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        FastTravel.Clear();
        EventBus.Clear();

        float lastJump = -1f;
        bool? lastOver = null;
        using var s1 = EventBus.Subscribe<GravJumped>(e => lastJump = e.DistanceLy);
        using var s2 = EventBus.Subscribe<CargoOverCapacityChanged>(e => lastOver = e.Over);

        var ship = new ShipSystems
        {
            MaxFuel = 100f,
            FuelPerLightYear = 4f,   // 100 fuel buys 25 ly, but range caps it
            JumpRangeLy = 18f,
            CargoCapacity = 600f,
        };
        ModHost.Register(ship);

        check.Equal("launches with a full tank", 100f, ship.Fuel);
        check.Equal("reach is the drive's range, not the fuel", 18f, ship.ReachableLy);

        // A jump within range spends fuel (10 ly * 4 = 40).
        check.That("a reachable jump fires", ship.Jump(10f));
        check.Equal("fuel spent on the jump", 60f, ship.Fuel);
        check.Equal("a jump event fired", 10f, lastJump);

        // Beyond the drive's range is refused even with fuel to spare.
        check.That("cannot jump past the drive's range", !ship.Jump(20f));
        check.Equal("a refused jump spends no fuel", 60f, ship.Fuel);

        // With 60 fuel the reach is now fuel-limited to 15 ly (60 / 4).
        check.Equal("reach is now fuel-limited", 15f, ship.ReachableLy);
        check.That("cannot jump past the fuel", !ship.Jump(16f));
        check.That("can jump to the fuel limit", ship.Jump(15f));
        check.Equal("the tank is dry", 0f, ship.Fuel);
        check.That("a dry tank reaches nothing", !ship.Jump(1f));

        // Refueling tops up without overflowing the tank.
        ship.Refuel(40f);
        check.Equal("refuel adds fuel", 40f, ship.Fuel);
        ship.Refuel(1000f);
        check.Equal("refuel never overflows", 100f, ship.Fuel);

        // Loading the hold past capacity grounds the drive and blocks fast travel.
        ship.Load(700f);
        check.That("the hold is over capacity", ship.IsOverCapacity);
        check.That("an over-mass gate event fired", lastOver == true);
        check.That("an over-mass hold grounds the drive", !ship.Jump(5f));
        check.Equal("a grounded drive reaches nothing", 0f, ship.ReachableLy);
        check.That("fast travel blocked while overloaded", !fake.FastTravelEnabled);

        // Trimming the hold below capacity frees the drive and reopens fast travel.
        ship.Unload(200f);
        check.That("trimmed below capacity", !ship.IsOverCapacity);
        check.That("the gate cleared", lastOver == false);
        check.That("the drive flies again", ship.Jump(5f));
        check.That("fast travel restored after trimming", fake.FastTravelEnabled);

        ModHost.Shutdown();
        FastTravel.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
