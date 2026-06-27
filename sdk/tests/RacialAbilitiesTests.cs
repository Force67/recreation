using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the racial-ability system: it grants the player's race passives once the
// player is loaded, and never re-grants them.
public static class RacialAbilitiesTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong race = 0x60, baseNpc = 0x50, frostResist = 0x70, resistEffect = 0x80;
        fake.SetSpellInfo(frostResist, cost: 0, type: 4);  // Ability (constant effect)
        fake.SetMagicEffectInfo(resistEffect, "ResistFrost", detrimental: false);
        fake.SetIngredientEffects(frostResist, (resistEffect, 50, 0));
        fake.SetRaceSpells(race, frostResist);
        fake.SetActorBaseData(baseNpc, sex: 0, race: race);
        fake.SetBase(fake.Player, baseNpc, essential: false);
        fake.SetValue(fake.Player, "ResistFrost", current: 0, baseValue: 100);

        ModHost.Register(new RacialAbilities());

        // The first tick grants the passive once the player and race resolve.
        ModHost.Tick(0.1f);
        check.Equal("racial passive granted on load", 50f, fake.GetCurrent(fake.Player, "ResistFrost"));

        // Further ticks do not re-grant it.
        ModHost.Tick(0.1f);
        ModHost.Tick(0.1f);
        check.Equal("not granted again", 50f, fake.GetCurrent(fake.Player, "ResistFrost"));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
