using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers diseases: contracting a disease-typed spell saps the victim's values,
// only disease spells count, and a cure lifts every ailment and reverts its
// effects.
public static class DiseasesTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Diseases.Clear();
        EventBus.Clear();

        // Rockjoint: a disease that weakens One-Handed while carried.
        const ulong rockjoint = 0xC00, weakenMelee = 0xC10;
        fake.SetMagicEffectInfo(weakenMelee, ActorValue.OneHanded, detrimental: true);
        fake.SetIngredientEffects(rockjoint, (weakenMelee, 25, 0));
        fake.SetSpellInfo(rockjoint, cost: 0, type: (int)SpellType.Disease);

        // A plain spell is not a disease and cannot be contracted.
        const ulong firebolt = 0xC01;
        fake.SetSpellInfo(firebolt, cost: 20, type: (int)SpellType.Spell);

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.OneHanded, current: 30, baseValue: 30);

        int contracted = 0, cured = 0;
        using var s1 = EventBus.Subscribe<DiseaseContracted>(_ => contracted++);
        using var s2 = EventBus.Subscribe<DiseasesCured>(e => cured += e.Count);

        check.That("contracting Rockjoint succeeds", Diseases.Contract(player, Spell.From(rockjoint)));
        check.Equal("the disease saps the skill", 5f, fake.GetCurrent(player.Handle, ActorValue.OneHanded));
        check.That("the actor carries it", Diseases.Has(player, Spell.From(rockjoint)));
        check.Equal("one disease carried", 1, Diseases.Count(player));

        check.That("contracting it again does nothing", !Diseases.Contract(player, Spell.From(rockjoint)));
        check.That("a plain spell is not a disease", !Diseases.Contract(player, Spell.From(firebolt)));
        check.Equal("only the one contraction fired", 1, contracted);

        check.Equal("the cure lifts the one disease", 1, Diseases.Cure(player));
        check.Equal("the skill is restored", 30f, fake.GetCurrent(player.Handle, ActorValue.OneHanded));
        check.Equal("no diseases remain", 0, Diseases.Count(player));
        check.Equal("a cure event reported the count", 1, cured);
        check.Equal("curing a clean actor cures nothing", 0, Diseases.Cure(player));

        ModHost.Shutdown();
        Diseases.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
