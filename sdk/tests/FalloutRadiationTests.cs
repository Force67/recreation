using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Fallout 4 radiation loop: rads accumulate in a hot field and floor at
// max, each rad shaves the same amount off effective max Health, stage events fire
// on crossings, RadAway clears rads and restores the cap, and decontamination wipes
// it all.
public static class FalloutRadiationTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Effects.Clear();
        EventBus.Clear();

        // The player starts with a full 100 Health bar; rads eat into it.
        fake.SetValue(fake.Player, ActorValue.Health, 1000, 1000);

        var stages = 0;
        RadiationStage stage = RadiationStage.Clean;
        using var sub = EventBus.Subscribe<RadiationStageChanged>(e => { stage = e.Stage; stages++; });

        var rad = new Radiation
        {
            RadsPerSecond = 100f,
            CurePerDose = 300f,
            MinorAt = 200f,
            AdvancedAt = 500f,
            CriticalAt = 800f,
            MaxRads = 1000f,
            RadAwayKeyword = FalloutForms.RadAwayKeyword,
        };
        ModHost.Register(rad);
        check.Equal("starts clean", 0f, rad.Rads);
        check.Equal("full health uncapped", 1000f, fake.GetCurrent(fake.Player, ActorValue.Health));

        // No field: rads do not climb even on a tick.
        ModHost.Tick(1f);
        check.Equal("no rads without a field", 0f, rad.Rads);

        // Enter a hot field: 3s -> 300 rads, into Minor, and Health capped by 300.
        rad.Irradiated = true;
        ModHost.Tick(3f);
        check.Equal("rads accumulate in a field", 300f, rad.Rads);
        check.Equal("crossed into Minor", RadiationStage.Minor, stage);
        check.Equal("one stage change so far", 1, stages);
        check.Equal("rads eat into max health", 700f, fake.GetCurrent(fake.Player, ActorValue.Health));

        // Push to Critical, then floor at MaxRads.
        ModHost.Tick(6f);  // would be 900, into Critical
        check.Equal("crossed into Critical", RadiationStage.Critical, stage);
        ModHost.Tick(5f);  // would overshoot 1000, floors there
        check.Equal("rads floor at max", 1000f, rad.Rads);
        check.Equal("max health fully eaten", 0f, fake.GetCurrent(fake.Player, ActorValue.Health));

        rad.Irradiated = false;

        // A RadAway-keyworded item clears CurePerDose rads and restores the cap.
        const ulong radaway = 0xB900;
        fake.SetHasKeyword(radaway, Game.GetForm(FalloutForms.RadAwayKeyword).Handle);
        EventBus.Publish(new ItemAdded(fake.Player, radaway, 1));
        check.Equal("RadAway removes a dose of rads", 700f, rad.Rads);
        check.Equal("the health cap eases with the cure", 300f, fake.GetCurrent(fake.Player, ActorValue.Health));

        // A non-RadAway item does nothing.
        const ulong scrap = 0xB901;
        EventBus.Publish(new ItemAdded(fake.Player, scrap, 1));
        check.Equal("a plain item does not cure rads", 700f, rad.Rads);

        // Decontamination wipes all rads and the whole cap.
        rad.Decontaminate();
        check.Equal("decontamination clears all rads", 0f, rad.Rads);
        check.Equal("back to Clean", RadiationStage.Clean, stage);
        check.Equal("health fully restored", 1000f, fake.GetCurrent(fake.Player, ActorValue.Health));

        ModHost.Shutdown();
        Effects.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
