using System.Linq;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers applying a race's traits: passive abilities fortify the actor's values,
// the racial power is surfaced rather than applied, and revoking reverts.
public static class RaceTraitsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong race = 0x60, baseNpc = 0x50;
        const ulong frostResist = 0x70, battleCry = 0x71, resistEffect = 0x80;
        fake.SetSpellInfo(frostResist, cost: 0, type: 4);  // Ability (constant effect)
        fake.SetSpellInfo(battleCry, cost: 0, type: 2);    // Power (once a day)
        fake.SetMagicEffectInfo(resistEffect, "ResistFrost", detrimental: false);
        fake.SetIngredientEffects(frostResist, (resistEffect, 50, 0));
        fake.SetRaceSpells(race, frostResist, battleCry);
        fake.SetRaceSkillBonuses(race, ("TwoHanded", 10), ("OneHanded", 5));
        fake.SetActorBaseData(baseNpc, sex: 0, race: race);
        fake.SetBase(fake.Player, baseNpc, essential: false);

        Actor player = Game.Player;
        fake.SetValue(player.Handle, "ResistFrost", current: 0, baseValue: 100);

        // Granting applies the passive ability, not the power.
        check.Equal("one passive granted", 1, RaceTraits.Grant(player));
        check.Equal("the passive fortifies the value", 50f, fake.GetCurrent(player.Handle, "ResistFrost"));

        // Granting again is idempotent.
        check.Equal("re-granting applies nothing", 0, RaceTraits.Grant(player));
        check.Equal("no double fortify", 50f, fake.GetCurrent(player.Handle, "ResistFrost"));

        // The power is surfaced for binding, not applied as a passive.
        var powers = RaceTraits.Powers(player).ToList();
        check.Equal("the racial power is surfaced", 1, powers.Count);
        check.Equal("the power is the battle cry", battleCry, powers[0].Handle);

        // Revoking reverts the passive.
        RaceTraits.Revoke(player);
        check.Equal("revoking reverts the value", 0f, fake.GetCurrent(player.Handle, "ResistFrost"));

        // Starting skill bonuses read and apply.
        var bonuses = player.Race.SkillBonuses;
        check.Equal("two skill bonuses", 2, bonuses.Count);
        check.Equal("the favoured skill is two-handed", "TwoHanded", bonuses[0].Skill);
        check.Equal("the specialty bonus is +10", 10, bonuses[0].Bonus);

        fake.SetValue(player.Handle, "TwoHanded", current: 15, baseValue: 15);
        fake.SetValue(player.Handle, "OneHanded", current: 20, baseValue: 20);
        RaceTraits.GrantSkillBonuses(player);
        check.Equal("two-handed raised by the racial bonus", 25f, fake.GetCurrent(player.Handle, "TwoHanded"));
        check.Equal("one-handed raised by the racial bonus", 25f, fake.GetCurrent(player.Handle, "OneHanded"));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
