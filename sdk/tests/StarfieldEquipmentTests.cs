using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers Starfield gear categorisation: a weapon's damage family and whether it
// draws ammo, an apparel piece's slot, and recognising ammunition, all from the
// type keywords on the record.
public static class StarfieldEquipmentTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong rifle = 0x600, laser = 0x601, knife = 0x602, plain = 0x603;
        const ulong suit = 0x604, helmet = 0x605, rounds = 0x606;
        // The fake maps GetForm(id) to the low 32 bits, so a keyword handle is just
        // the placeholder keyword id from StarfieldForms.
        fake.SetHasKeyword(rifle, StarfieldForms.WeaponTypeBallistic);
        fake.SetHasKeyword(laser, StarfieldForms.WeaponTypeLaser);
        fake.SetHasKeyword(knife, StarfieldForms.WeaponTypeMelee);
        fake.SetHasKeyword(suit, StarfieldForms.ApparelSpacesuit);
        fake.SetHasKeyword(helmet, StarfieldForms.ApparelHelmet);
        fake.SetHasKeyword(rounds, StarfieldForms.AmmoKeyword);

        // Weapon families.
        check.Equal("a rifle reads ballistic", WeaponKind.Ballistic,
                    StarfieldEquipment.KindOf(Form.From(rifle)));
        check.Equal("a laser reads laser", WeaponKind.Laser,
                    StarfieldEquipment.KindOf(Form.From(laser)));
        check.Equal("a knife reads melee", WeaponKind.Melee,
                    StarfieldEquipment.KindOf(Form.From(knife)));
        check.Equal("a keywordless form is no weapon", WeaponKind.None,
                    StarfieldEquipment.KindOf(Form.From(plain)));
        check.That("a rifle is a weapon", StarfieldEquipment.IsWeapon(Form.From(rifle)));

        // Ammo use: ranged families draw ammo, melee does not.
        check.That("a ballistic weapon uses ammo", StarfieldEquipment.UsesAmmo(Form.From(rifle)));
        check.That("a laser uses ammo", StarfieldEquipment.UsesAmmo(Form.From(laser)));
        check.That("a melee weapon uses no ammo", !StarfieldEquipment.UsesAmmo(Form.From(knife)));

        // Apparel slots.
        check.Equal("a spacesuit reads the suit slot", ApparelSlot.Spacesuit,
                    StarfieldEquipment.SlotOf(Form.From(suit)));
        check.Equal("a helmet reads the helmet slot", ApparelSlot.Helmet,
                    StarfieldEquipment.SlotOf(Form.From(helmet)));
        check.Equal("clothing has no slot", ApparelSlot.None,
                    StarfieldEquipment.SlotOf(Form.From(plain)));
        check.That("a spacesuit is apparel", StarfieldEquipment.IsApparel(Form.From(suit)));
        check.That("a weapon is not apparel", !StarfieldEquipment.IsApparel(Form.From(rifle)));

        // Ammunition.
        check.That("rounds read as ammo", StarfieldEquipment.IsAmmo(Form.From(rounds)));
        check.That("a rifle is not ammo", !StarfieldEquipment.IsAmmo(Form.From(rifle)));

        Native.Backend = null;
    }
}
