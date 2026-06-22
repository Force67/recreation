using System;
using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers Fallout 4 weapon damage: melee damage rises with Strength (+10% per
// point), ranged damage rises with the weapon perk's rank (+20% per rank), and
// the damage type is read from the weapon's channel keyword.
public static class FalloutWeaponDamageTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // Melee: +10% per Strength point above 1.
        Near(check, "melee multiplier at Strength 1 is flat", 1f, WeaponDamage.MeleeMultiplier(1));
        Near(check, "melee multiplier at Strength 10 is +90%", 1.9f, WeaponDamage.MeleeMultiplier(10));
        Near(check, "Strength 10 turns a 50-dmg blade into 95", 95f, WeaponDamage.MeleeDamage(50, 10));
        check.Equal("no base, no damage", 0f, WeaponDamage.MeleeDamage(0, 10));

        // Ranged: +20% per perk rank.
        Near(check, "ranged multiplier at rank 0 is flat", 1f, WeaponDamage.RangedMultiplier(0));
        Near(check, "ranged multiplier at rank 3 is +60%", 1.6f, WeaponDamage.RangedMultiplier(3));
        Near(check, "rank 3 turns a 30-dmg rifle into 48", 48f, WeaponDamage.RangedDamage(30, 3));

        // Damage type from the weapon's channel keyword; physical when none.
        const ulong ballistic = 0x900, laser = 0x901, gamma = 0x902;
        fake.SetHasKeyword(laser, FalloutForms.DamageTypeEnergy);
        fake.SetHasKeyword(gamma, FalloutForms.DamageTypeRadiation);
        check.Equal("a bare weapon is physical", DamageType.Physical,
                    WeaponDamage.TypeOf(Form.From(ballistic)));
        check.Equal("an energy weapon reads its keyword", DamageType.Energy,
                    WeaponDamage.TypeOf(Form.From(laser)));
        check.Equal("a rad weapon reads its keyword", DamageType.Radiation,
                    WeaponDamage.TypeOf(Form.From(gamma)));

        // Convenience over the record: a melee weapon scaled by the wielder's
        // Strength.
        const ulong sledge = 0x910;
        fake.SetWeaponDamage(sledge, 60);
        fake.SetValue(fake.Player, FalloutActorValue.Strength, current: 6, baseValue: 6);
        Near(check, "a 60-dmg sledge at Strength 6 deals 90", 90f,
             WeaponDamage.MeleeDamage(Form.From(sledge), Game.Player));

        Native.Backend = null;
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
