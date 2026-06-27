using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Starfield O2/CO2 survival loop: oxygen drains while exerting and
// floors at 0, CO2 builds only once oxygen is gone and decays when it returns,
// stage events fire exactly on crossings, and config thresholds are respected.
public static class StarfieldOxygenTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();

        var o2Stages = 0;
        OxygenStage o2Stage = OxygenStage.Full;
        var co2Stages = 0;
        CarbonDioxideStage co2Stage = CarbonDioxideStage.Clear;
        using var s1 = EventBus.Subscribe<OxygenStageChanged>(e => { o2Stage = e.Stage; o2Stages++; });
        using var s2 = EventBus.Subscribe<CarbonDioxideStageChanged>(e => { co2Stage = e.Stage; co2Stages++; });

        var lungs = new OxygenCo2
        {
            DrainPerSecond = 25f,
            RegenPerSecond = 25f,
            RestorePerSource = 60f,
            CarbonDioxideRisePerSecond = 10f,
            CarbonDioxideDecayPerSecond = 20f,
            OxygenLowAt = 35f,
            CarbonDioxideBuildingAt = 25f,
            HypoxiaAfterSeconds = 1000f,  // out of reach: this suite is about the meters
            OxygenSourceKeyword = StarfieldForms.OxygenSourceKeyword,
        };
        ModHost.Register(lungs);
        check.Equal("starts with full oxygen", 100f, lungs.Oxygen);

        // Exert for 3s: 75 oxygen spent, dropping below OxygenLowAt into Low.
        lungs.Exerting = true;
        ModHost.Tick(3f);
        check.Equal("oxygen drains while exerting", 25f, lungs.Oxygen);
        check.Equal("crossed into Low", OxygenStage.Low, o2Stage);
        check.Equal("one oxygen stage change so far", 1, o2Stages);
        check.Equal("no CO2 while oxygen remains", 0f, lungs.CarbonDioxide);
        // The breath meter reaches the HUD as a gauge tracking the live value.
        check.That("oxygen gauge pushed to HUD",
            fake.Gauges.TryGetValue("oxygen", out var og) && og > 0.24f && og < 0.26f);

        // Two more seconds of exertion floors oxygen at 0 (Empty), and CO2 starts.
        ModHost.Tick(2f);
        check.Equal("oxygen floors at zero", 0f, lungs.Oxygen);
        check.Equal("crossed into Empty", OxygenStage.Empty, o2Stage);
        check.Equal("two oxygen stage changes", 2, o2Stages);
        // Oxygen hit 0 partway through the tick, so CO2 only accrues for the rest.
        check.That("CO2 begins building once oxygen is gone", lungs.CarbonDioxide > 0f);

        // Three more empty seconds push CO2 past the Building threshold.
        ModHost.Tick(3f);
        check.Equal("CO2 reaches Building", CarbonDioxideStage.Building, co2Stage);
        check.Equal("one CO2 stage change so far", 1, co2Stages);

        // Stop exerting: oxygen regenerates and CO2 decays back toward Clear.
        lungs.Exerting = false;
        ModHost.Tick(2f);
        check.Equal("oxygen regenerates at rest", 50f, lungs.Oxygen);
        check.That("CO2 decays when oxygen returns", lungs.CarbonDioxide < 30f);

        // Consuming an oxygen-source item tops oxygen back up.
        const ulong canister = 0x900;
        fake.SetHasKeyword(canister, Game.GetForm(StarfieldForms.OxygenSourceKeyword).Handle);
        EventBus.Publish(new ItemAdded(fake.Player, canister, 1));
        check.Equal("an oxygen source refills oxygen", 100f, lungs.Oxygen);

        // A non-oxygen item does nothing.
        const ulong rock = 0x901;
        EventBus.Publish(new ItemAdded(fake.Player, rock, 1));
        check.Equal("a plain item does not refill", 100f, lungs.Oxygen);

        // Combat counts as exertion: oxygen drains in a fight with no manual flag.
        fake.SetInCombat(fake.Player, true);
        ModHost.Tick(1f);
        check.That("combat drains oxygen", lungs.Oxygen < 100f);
        fake.SetInCombat(fake.Player, false);

        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
