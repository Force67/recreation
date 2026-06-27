using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield weapon-applied status effects: a hit lays its status through
// the Afflictions registry (sapping the right actor value), a second application
// of the same status is a no-op, and clearing it reverts just that drain.
public static class StarfieldCombatStatusTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();

        Actor target = Game.Player;
        fake.SetValue(target.Handle, ActorValue.Health, current: 100, baseValue: 100);
        fake.SetValue(target.Handle, ActorValue.SpeedMult, current: 100, baseValue: 100);

        // An EM hit staggers: SpeedMult drops by the stagger magnitude.
        check.That("an EM hit applies stagger", CombatStatusEffects.ApplyOnHit(target,
                   CombatStatusEffects.Staggered));
        check.Equal("stagger saps SpeedMult", 70f,
                    fake.GetCurrent(target.Handle, ActorValue.SpeedMult));
        check.That("the target carries the stagger", Afflictions.Has(target, "Staggered"));

        // The same status does not stack a second time.
        check.That("a second stagger is a no-op", !CombatStatusEffects.ApplyOnHit(target,
                   CombatStatusEffects.Staggered));
        check.Equal("the drain did not double", 70f,
                    fake.GetCurrent(target.Handle, ActorValue.SpeedMult));

        // A burn composes alongside it on a different actor value.
        check.That("an incendiary hit applies burning", CombatStatusEffects.ApplyOnHit(target,
                   CombatStatusEffects.Burning));
        check.Equal("burning saps Health", 90f, fake.GetCurrent(target.Handle, ActorValue.Health));

        // Clearing the stagger reverts just its drain, leaving the burn.
        check.That("the stagger clears", CombatStatusEffects.Clear(target,
                   CombatStatusEffects.Staggered));
        check.Equal("SpeedMult restored", 100f,
                    fake.GetCurrent(target.Handle, ActorValue.SpeedMult));
        check.That("the burn still holds", Afflictions.Has(target, "Burning"));
        check.Equal("Health still drained by the burn", 90f,
                    fake.GetCurrent(target.Handle, ActorValue.Health));

        // The catalogue lands on the channels the weapons that carry them deal.
        check.Equal("stagger is an EM status", StarfieldDamageType.Electromagnetic,
                    CombatStatusEffects.Staggered.Channel);
        check.Equal("burning is an energy status", StarfieldDamageType.Energy,
                    CombatStatusEffects.Burning.Channel);

        ModHost.Shutdown();
        Afflictions.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
