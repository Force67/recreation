using System.Linq;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers race abilities: reading a race's innate spells, telling abilities from
// powers, reaching them through an actor, and applying a racial passive.
public static class RaceTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong nordRace = 0x60, npcBase = 0x50;
        const ulong frostResist = 0x70, battleCry = 0x71, resistEffect = 0x80;
        fake.SetSpellInfo(frostResist, cost: 0, type: 4);  // 4 = Ability (constant effect)
        fake.SetSpellInfo(battleCry, cost: 0, type: 2);    // 2 = Power (once a day)
        fake.SetMagicEffectInfo(resistEffect, "ResistFrost", detrimental: false);
        fake.SetIngredientEffects(frostResist, (resistEffect, 50, 0));  // +50 ResistFrost
        fake.SetRaceSpells(nordRace, frostResist, battleCry);
        fake.SetActorBaseData(npcBase, sex: 0, race: nordRace);
        fake.SetBase(fake.Player, npcBase, essential: false);

        // The race lists its innate spells.
        Race race = Race.From(nordRace);
        check.Equal("the race lists its spells", 2, race.Abilities.Count);

        // Type tells an ability from a power.
        var passives = race.Abilities.Where(s => s.Type == SpellType.Ability).ToList();
        var powers = race.Abilities.Where(s => s.Type == SpellType.Power).ToList();
        check.Equal("one passive ability", 1, passives.Count);
        check.Equal("one racial power", 1, powers.Count);
        check.Equal("the passive is the frost resist", frostResist, passives[0].Handle);

        // An actor reaches its race's abilities through its base.
        Actor player = Game.Player;
        check.Equal("the actor resolves its race", nordRace, player.Race.Handle);
        check.Equal("the actor reaches its race abilities", 2, player.Race.Abilities.Count);

        // Composing: applying the racial passive fortifies the value.
        fake.SetValue(player.Handle, "ResistFrost", current: 0, baseValue: 100);
        Abilities.Apply(player, passives[0]);
        check.Equal("racial passive fortifies the value", 50f,
                    fake.GetCurrent(player.Handle, "ResistFrost"));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
