using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield afflictions: contracting one saps the actor value, a second
// contraction of the same condition is a no-op, a cure reverts the drain and
// reports the count, and the O2 -> CO2 -> affliction cascade really inflicts
// hypoxia end to end.
public static class StarfieldAfflictionsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Health, current: 100, baseValue: 100);

        int contracted = 0, cured = 0;
        using var s1 = EventBus.Subscribe<AfflictionContracted>(_ => contracted++);
        using var s2 = EventBus.Subscribe<AfflictionsCured>(e => cured += e.Count);

        var hypoxia = new Affliction("Hypoxia", ActorValue.Health, 20f);

        check.That("contracting hypoxia succeeds", Afflictions.Contract(player, hypoxia));
        check.Equal("the affliction saps Health", 80f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.That("the actor carries it", Afflictions.Has(player, "Hypoxia"));
        check.Equal("one affliction carried", 1, Afflictions.Count(player));

        check.That("contracting it again does nothing", !Afflictions.Contract(player, hypoxia));
        check.Equal("only the one contraction fired", 1, contracted);
        check.Equal("the double contract did not stack the drain", 80f,
                    fake.GetCurrent(player.Handle, ActorValue.Health));

        check.Equal("the cure lifts the one affliction", 1, Afflictions.Cure(player));
        check.Equal("Health is restored", 100f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("no afflictions remain", 0, Afflictions.Count(player));
        check.Equal("a cure event reported the count", 1, cured);
        check.Equal("curing a clean actor cures nothing", 0, Afflictions.Cure(player));

        // The cascade: sustained maxed CO2 from OxygenCo2 inflicts hypoxia, and
        // clearing the debt cures it.
        var lungs = new OxygenCo2
        {
            DrainPerSecond = 100f,            // one tick empties oxygen
            RegenPerSecond = 100f,
            CarbonDioxideRisePerSecond = 100f,  // and the next maxes CO2
            CarbonDioxideDecayPerSecond = 100f,
            HypoxiaAfterSeconds = 2f,
            Exerting = true,
        };
        ModHost.Register(lungs);

        // The first tick empties oxygen and maxes CO2 at once, so the grace clock
        // starts here: 1s maxed, still under HypoxiaAfterSeconds.
        ModHost.Tick(1f);
        check.That("no hypoxia before the grace period", !Afflictions.Has(player, "Hypoxia"));

        // Another second of maxed CO2 reaches the grace period and hypoxia sets in.
        ModHost.Tick(1f);
        check.That("sustained maxed CO2 inflicts hypoxia", Afflictions.Has(player, "Hypoxia"));
        check.Equal("the cascade sapped Health", 80f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // Breathing again (rest) decays CO2 and the cure fires automatically.
        lungs.Exerting = false;
        ModHost.Tick(1f);  // oxygen returns
        ModHost.Tick(1f);  // CO2 leaves Maxed
        check.That("clearing CO2 cures hypoxia", !Afflictions.Has(player, "Hypoxia"));
        check.Equal("Health restored after the cure", 100f, fake.GetCurrent(player.Handle, ActorValue.Health));

        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
