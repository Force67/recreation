using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the standing stones: taking one applies its blessing, re-touching the
// same stone is a no-op, and switching stones drops the old blessing for the new,
// raising an event each real change.
public static class StandingStonesTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        StandingStones.Clear();
        EventBus.Clear();

        // Two stones, each fortifying a different skill while held.
        const ulong warrior = 0xB00, mage = 0xB01;
        const ulong fortifyOneHanded = 0xB10, fortifyDestruction = 0xB11;
        fake.SetMagicEffectInfo(fortifyOneHanded, ActorValue.OneHanded, detrimental: false);
        fake.SetMagicEffectInfo(fortifyDestruction, ActorValue.Destruction, detrimental: false);
        fake.SetIngredientEffects(warrior, (fortifyOneHanded, 15, 0));
        fake.SetIngredientEffects(mage, (fortifyDestruction, 15, 0));

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.OneHanded, current: 20, baseValue: 20);
        fake.SetValue(player.Handle, ActorValue.Destruction, current: 20, baseValue: 20);

        check.Equal("no stone held initially", 0UL, StandingStones.Current(player));

        int changes = 0;
        using var sub = EventBus.Subscribe<StandingStoneChanged>(_ => changes++);

        // Taking the Warrior fortifies One-Handed.
        check.That("taking the Warrior succeeds", StandingStones.Activate(player, Spell.From(warrior)));
        check.Equal("Warrior is current", warrior, StandingStones.Current(player));
        check.Equal("One-Handed fortified", 35f, fake.GetCurrent(player.Handle, ActorValue.OneHanded));

        // Re-touching the same stone changes nothing.
        check.That("re-touching the Warrior is a no-op", !StandingStones.Activate(player, Spell.From(warrior)));
        check.Equal("no double fortify", 35f, fake.GetCurrent(player.Handle, ActorValue.OneHanded));

        // Switching to the Mage drops the Warrior blessing and applies the Mage's.
        check.That("switching to the Mage succeeds", StandingStones.Activate(player, Spell.From(mage)));
        check.Equal("Mage is current", mage, StandingStones.Current(player));
        check.Equal("One-Handed reverts when the Warrior is dropped", 20f,
                    fake.GetCurrent(player.Handle, ActorValue.OneHanded));
        check.Equal("Destruction fortified", 35f, fake.GetCurrent(player.Handle, ActorValue.Destruction));
        check.That("Warrior no longer held", !StandingStones.Holds(player, Spell.From(warrior)));

        check.Equal("one event per real change", 2, changes);

        ModHost.Shutdown();
        StandingStones.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
