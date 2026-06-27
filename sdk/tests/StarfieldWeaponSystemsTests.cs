using System;
using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers Starfield weapon systems: a shot holds full damage inside its effective
// range then falls off linearly to a floor at maximum range, headshots multiply
// the impact, each firing class carries its own range band, and the damage
// channel is read from the weapon's class.
public static class StarfieldWeaponSystemsTests
{
    public static void Run(Check check)
    {
        // Range falloff: full inside effective range, linear ramp to the floor at
        // maximum range (here 1500 effective, 4000 max, 0.4 floor).
        Near(check, "full damage inside effective range", 100f,
             WeaponSystems.AfterFalloff(100, 1000, 1500, 4000));
        Near(check, "half-way to max range loses 30%", 70f,
             WeaponSystems.AfterFalloff(100, 2750, 1500, 4000));
        Near(check, "at max range the floor remains", 40f,
             WeaponSystems.AfterFalloff(100, 4000, 1500, 4000));
        Near(check, "past max range stays at the floor", 40f,
             WeaponSystems.AfterFalloff(100, 9000, 1500, 4000));
        check.Equal("no base, no damage", 0f, WeaponSystems.AfterFalloff(0, 100, 1500, 4000));

        // Energy classes keep full damage farther than ballistics.
        check.Equal("a laser's effective range", 2500f,
                    WeaponSystems.RangeFor(StarfieldWeaponClass.Laser).Effective);
        check.Equal("a particle beam's max range", 6000f,
                    WeaponSystems.RangeFor(StarfieldWeaponClass.Particle).Max);

        // A point-blank shot: full damage, doubled on a headshot.
        Near(check, "a point-blank ballistic shot is full", 50f,
             WeaponSystems.ShotDamage(50, StarfieldWeaponClass.Ballistic, distance: 200));
        Near(check, "a headshot doubles the impact", 100f,
             WeaponSystems.ShotDamage(50, StarfieldWeaponClass.Ballistic, distance: 200,
                                      headshot: true));

        // Class -> channel mapping (used when a record has no explicit keyword).
        check.Equal("ballistic lands physical", StarfieldDamageType.Physical,
                    StarfieldEquipment.ChannelFor(StarfieldWeaponClass.Ballistic));
        check.Equal("laser lands energy", StarfieldDamageType.Energy,
                    StarfieldEquipment.ChannelFor(StarfieldWeaponClass.Laser));
        check.Equal("EM lands electromagnetic", StarfieldDamageType.Electromagnetic,
                    StarfieldEquipment.ChannelFor(StarfieldWeaponClass.Electromagnetic));

        // The keyword reads route through a Starfield world.
        Domains.Clear();
        var primary = new FakeBackend();
        Domains.Register(new GameWorld("Skyrim Special Edition", primary), isPrimary: true);
        var sf = new FakeBackend();
        const ulong laser = 0xC100, emGun = 0xC101, bare = 0xC102;
        sf.SetHasKeyword(laser, StarfieldForms.WeaponClassLaser);
        sf.SetHasKeyword(emGun, StarfieldForms.WeaponClassEm);
        sf.SetHasKeyword(emGun, StarfieldForms.DamageTypeElectromagnetic);
        Domains.Register(new GameWorld(Starfield.GameName, sf), isPrimary: false);

        check.Equal("a laser weapon reads its class", StarfieldWeaponClass.Laser,
                    StarfieldEquipment.ClassOf(Starfield.GetForm((uint)laser)));
        check.Equal("a laser lands on the energy channel", StarfieldDamageType.Energy,
                    StarfieldEquipment.DamageTypeOf(Starfield.GetForm((uint)laser)));
        check.Equal("an EM weapon reads its explicit channel", StarfieldDamageType.Electromagnetic,
                    StarfieldEquipment.DamageTypeOf(Starfield.GetForm((uint)emGun)));
        check.Equal("an unclassified weapon is physical", StarfieldDamageType.Physical,
                    StarfieldEquipment.DamageTypeOf(Starfield.GetForm((uint)bare)));

        Domains.Clear();
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
