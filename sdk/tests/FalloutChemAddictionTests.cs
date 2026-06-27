using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers chem addiction: a use can hook the player based on the roll, withdrawal
// saps an actor value while carried, a chem already addicted to does not re-hook,
// curing one reverts just its withdrawal, and Addictol cures every addiction at once.
public static class FalloutChemAddictionTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ChemAddiction.Clear();
        Effects.Clear();
        EventBus.Clear();

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Stamina, 100, 100);
        fake.SetValue(player.Handle, ActorValue.Health, 100, 100);

        // Jet: a 50% chance to hook, withdrawal saps Stamina by 20.
        var jet = new Chem("Jet", ActorValue.Stamina, 20f, addictionChance: 0.5f);
        // Buffout: a 30% chance, withdrawal saps Health by 15.
        var buffout = new Chem("Buffout", ActorValue.Health, 15f, addictionChance: 0.3f);

        int addicted = 0, cured = 0;
        using var s1 = EventBus.Subscribe<ChemAddicted>(_ => addicted++);
        using var s2 = EventBus.Subscribe<AddictionCured>(_ => cured++);

        // A roll above the chance does not hook (lucky use).
        check.That("a high roll does not hook", !ChemAddiction.Use(player, jet, roll: 0.9f));
        check.Equal("no addiction yet", 0, ChemAddiction.Count(player));
        check.Equal("stamina untouched", 100f, fake.GetCurrent(player.Handle, ActorValue.Stamina));

        // A roll within the chance hooks the player; withdrawal saps Stamina.
        check.That("a low roll hooks the player", ChemAddiction.Use(player, jet, roll: 0.1f));
        check.That("addicted to Jet", ChemAddiction.Has(player, "Jet"));
        check.Equal("withdrawal saps stamina", 80f, fake.GetCurrent(player.Handle, ActorValue.Stamina));
        check.Equal("one addiction event", 1, addicted);

        // Using Jet again while addicted does not re-hook or stack the drain.
        check.That("an addicted chem does not re-hook", !ChemAddiction.Use(player, jet, roll: 0.0f));
        check.Equal("drain not stacked", 80f, fake.GetCurrent(player.Handle, ActorValue.Stamina));

        // A second chem hooks independently.
        check.That("Buffout hooks too", ChemAddiction.Use(player, buffout, roll: 0.2f));
        check.Equal("two addictions carried", 2, ChemAddiction.Count(player));
        check.Equal("buffout saps health", 85f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // Curing one reverts just its withdrawal.
        check.That("cure Jet", ChemAddiction.Cure(player, "Jet"));
        check.Equal("stamina restored", 100f, fake.GetCurrent(player.Handle, ActorValue.Stamina));
        check.Equal("health still sapped", 85f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("one addiction remains", 1, ChemAddiction.Count(player));
        check.Equal("one cure event", 1, cured);

        // Addictol cures the rest.
        check.Equal("Addictol cures all remaining", 1, ChemAddiction.CureAll(player));
        check.Equal("health restored", 100f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("no addictions remain", 0, ChemAddiction.Count(player));

        ChemAddiction.Clear();
        Effects.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
