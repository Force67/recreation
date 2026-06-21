using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield environmental hazards: standing unsealed in a hazard field
// builds exposure to a hazard-specific affliction, a sealed suit protects, and
// recovering to safe cures just that affliction.
public static class StarfieldHazardsTests
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

        int stages = 0;
        using var sub = EventBus.Subscribe<HazardStageChanged>(_ => stages++);

        var hazards = new EnvironmentalHazards { ExposurePerSecond = 100f, RecoveryPerSecond = 100f };
        ModHost.Register(hazards);

        // Unsealed in a radiation field: one second maxes exposure and inflicts it.
        hazards.Hazard = SpaceHazard.Radiation;
        hazards.Sealed = false;
        ModHost.Tick(1.0f);
        check.Equal("exposure maxes", 100f, hazards.Exposure);
        check.That("radiation poisoning contracted", Afflictions.Has(player, "RadiationPoisoning"));
        check.Equal("health sapped by the affliction", 75f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.That("a stage change fired", stages >= 1);

        // Sealing the suit lets exposure recover, curing just that affliction.
        hazards.Sealed = true;
        ModHost.Tick(1.0f);
        check.Equal("exposure recovers to zero", 0f, hazards.Exposure);
        check.That("the affliction is cured", !Afflictions.Has(player, "RadiationPoisoning"));
        check.Equal("health restored on cure", 100f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // A sealed suit blocks exposure entirely.
        hazards.Hazard = SpaceHazard.Thermal;
        hazards.Sealed = true;
        ModHost.Tick(1.0f);
        check.Equal("sealed suit takes no exposure", 0f, hazards.Exposure);
        check.Equal("no affliction while sealed", 0, Afflictions.Count(player));

        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
