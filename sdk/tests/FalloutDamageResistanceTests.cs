using System;
using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the Fallout 4 damage-resistance curve: a resistance blunts a hit by a
// ratio of the hit to the resistance (not a flat percent), every hit keeps a
// chip-damage floor, and the right channel is read for each damage type.
public static class FalloutDamageResistanceTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // No resistance lets the whole hit through.
        Near(check, "zero resist lets the full hit through", 100f, DamageResistance.After(100, 0));
        check.Equal("no hit, no damage", 0f, DamageResistance.After(0, 500));

        // Resistance equal to the hit lets just over three quarters through, and
        // the curve keeps biting as resistance climbs (it is a ratio, not linear).
        Near(check, "resist == damage lets ~77.5% through", 77.539f,
             DamageResistance.After(100, 100));
        Near(check, "triple resist lets ~60% through", 60.123f, DamageResistance.After(100, 300));

        // A bigger hit against the same resistance loses a smaller fraction: the
        // armor matters more the closer it sits to the incoming damage.
        Near(check, "a heavy hit sheds little to light resist", 184.274f,
             DamageResistance.After(200, 50));

        // Stacking resistance never grants immunity: a hit floors at the chip
        // fraction (1% of the raw here).
        Near(check, "overwhelming resist still chips for the floor", 0.1f,
             DamageResistance.After(10, 1_000_000_000f));

        // The through-fraction is what After multiplies by.
        Near(check, "through fraction at resist == damage", 0.77539f,
             DamageResistance.ThroughFraction(100, 100));

        // Each damage type reads its own resistance channel off the actor.
        Actor player = Game.Player;
        fake.SetValue(player.Handle, FalloutActorValue.DamageResist, current: 100, baseValue: 100);
        fake.SetValue(player.Handle, FalloutActorValue.EnergyResist, current: 300, baseValue: 300);
        fake.SetValue(player.Handle, FalloutActorValue.RadResist, current: 0, baseValue: 0);

        Near(check, "ballistic hit meets DamageResist", 77.539f,
             DamageResistance.AfterFor(player, 100, DamageType.Physical));
        Near(check, "energy hit meets EnergyResist", 60.123f,
             DamageResistance.AfterFor(player, 100, DamageType.Energy));
        Near(check, "rad hit meets RadResist (none here)", 100f,
             DamageResistance.AfterFor(player, 100, DamageType.Radiation));

        check.Equal("physical reads DamageResist", FalloutActorValue.DamageResist,
                    DamageResistance.ResistValueFor(DamageType.Physical));
        check.Equal("energy reads EnergyResist", FalloutActorValue.EnergyResist,
                    DamageResistance.ResistValueFor(DamageType.Energy));
        check.Equal("rad reads RadResist", FalloutActorValue.RadResist,
                    DamageResistance.ResistValueFor(DamageType.Radiation));

        Native.Backend = null;
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
