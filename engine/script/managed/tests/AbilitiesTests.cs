using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers passive abilities: applying one fortifies the actor's values for as long
// as it is held, re-applying is a no-op, removing reverts it, and teardown clears
// a lingering ability.
public static class AbilitiesTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong lordStone = 0x900, fortifyHealth = 0xA00;
        fake.SetMagicEffectInfo(fortifyHealth, "Health", detrimental: false);
        fake.SetIngredientEffects(lordStone, (fortifyHealth, 50, 0));  // constant-effect +50

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Health, current: 100, baseValue: 100);
        Spell ability = Spell.From(lordStone);

        // Applying fortifies the value and marks the ability active.
        check.That("apply grants the ability", Abilities.Apply(player, ability));
        check.Equal("value fortified", 150f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.That("ability is active", Abilities.IsActive(player, ability));
        check.Equal("one active ability", 1, Abilities.ActiveCount);

        // It cannot stack with itself.
        check.That("re-applying does nothing", !Abilities.Apply(player, ability));
        check.Equal("no double fortify", 150f, fake.GetCurrent(player.Handle, ActorValue.Health));

        // Removing reverts the value.
        check.That("remove succeeds", Abilities.Remove(player, ability));
        check.Equal("value reverted", 100f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.That("ability no longer active", !Abilities.IsActive(player, ability));
        check.That("removing again does nothing", !Abilities.Remove(player, ability));

        // A detrimental ability saps a value, and removing restores it.
        const ulong curse = 0x901, damageStamina = 0xA01;
        fake.SetMagicEffectInfo(damageStamina, "Stamina", detrimental: true);
        fake.SetIngredientEffects(curse, (damageStamina, 30, 0));
        fake.SetValue(player.Handle, ActorValue.Stamina, current: 100, baseValue: 100);
        Abilities.Apply(player, Spell.From(curse));
        check.Equal("a curse saps the value", 70f, fake.GetCurrent(player.Handle, ActorValue.Stamina));
        Abilities.Remove(player, Spell.From(curse));
        check.Equal("lifting it restores the value", 100f, fake.GetCurrent(player.Handle, ActorValue.Stamina));

        // Teardown reverts a still-active ability.
        Abilities.Apply(player, ability);
        check.Equal("granted again", 150f, fake.GetCurrent(player.Handle, ActorValue.Health));
        ModHost.Shutdown();
        check.Equal("teardown reverts the ability", 100f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("teardown clears abilities", 0, Abilities.ActiveCount);

        EventBus.Clear();
        Native.Backend = null;
    }
}
