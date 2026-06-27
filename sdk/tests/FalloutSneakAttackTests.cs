using System;
using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers Fallout 4 sneak-attack and VATS-crit damage: a sneak attack doubles
// damage (Ninja pushes ranged to 2.5x and melee toward a 10x finisher), a VATS
// crit adds the base damage again (raised by Better Criticals), and a crit is
// gated on having the Action Points to queue it.
public static class FalloutSneakAttackTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // Ranged sneak: 2x at base, 2.5x at Ninja rank 3.
        Near(check, "ranged sneak doubles at base", 2f, SneakAttack.RangedSneakMultiplier(0));
        Near(check, "ranged sneak reaches 2.5x at Ninja 3", 2.5f, SneakAttack.RangedSneakMultiplier(3));
        Near(check, "a 40-dmg sneak shot deals 100 at Ninja 3", 100f,
             SneakAttack.RangedSneakDamage(40, 3));

        // Melee sneak: 2x at base, 10x finisher at Ninja rank 3.
        Near(check, "melee sneak doubles at base", 2f, SneakAttack.MeleeSneakMultiplier(0));
        Near(check, "melee sneak reaches 10x at Ninja 3", 10f, SneakAttack.MeleeSneakMultiplier(3));
        Near(check, "a 40-dmg sneak swing deals 400 at Ninja 3", 400f,
             SneakAttack.MeleeSneakDamage(40, 3));

        // VATS crit: adds the base again (2x), Better Criticals raises it.
        Near(check, "a crit doubles at base", 2f, SneakAttack.CritMultiplier(0));
        Near(check, "Better Criticals 2 lifts a crit to 3x", 3f, SneakAttack.CritMultiplier(2));
        Near(check, "a 50-dmg crit deals 100", 100f, SneakAttack.CritDamage(50));
        Near(check, "a 50-dmg crit deals 150 at Better Criticals 2", 150f,
             SneakAttack.CritDamage(50, 2));

        check.Equal("no base, no sneak damage", 0f, SneakAttack.RangedSneakDamage(0, 3));

        // A VATS crit needs the AP to queue the shot.
        Actor player = Game.Player;
        fake.SetValue(player.Handle, FalloutActorValue.ActionPoints, current: 30, baseValue: 90);
        check.That("a crit is available with the AP for it", SneakAttack.CanVatsCrit(player));
        fake.SetValue(player.Handle, FalloutActorValue.ActionPoints, current: 10, baseValue: 90);
        check.That("a crit is gated when AP is too low", !SneakAttack.CanVatsCrit(player));

        Native.Backend = null;
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
