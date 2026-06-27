using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers equipment categorisation: a weapon's type and one/two-handedness, and an
// armor's weight class, read from the type keywords on the record.
public static class EquipmentTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong sword = 0x100, greatsword = 0x101, bow = 0x102;
        const ulong heavy = 0x103, light = 0x104, plain = 0x105;
        // The fake maps GetForm(id) to the low 32 bits, so keyword handles are the
        // low bits of the base-game keyword form ids.
        fake.SetHasKeyword(sword, 0x1E711);       // WeapTypeSword
        fake.SetHasKeyword(greatsword, 0x6D931);  // WeapTypeGreatsword
        fake.SetHasKeyword(bow, 0x1E715);         // WeapTypeBow
        fake.SetHasKeyword(heavy, 0x6BBD2);       // ArmorHeavy
        fake.SetHasKeyword(light, 0x6BBD3);       // ArmorLight

        // Weapon types.
        check.Equal("a sword reads as a sword", WeaponType.Sword, Equipment.TypeOf(Form.From(sword)));
        check.Equal("a greatsword reads as a greatsword", WeaponType.Greatsword,
                    Equipment.TypeOf(Form.From(greatsword)));
        check.Equal("a keywordless form is no weapon", WeaponType.None, Equipment.TypeOf(Form.From(plain)));

        // One- vs two-handed.
        check.That("a sword is one-handed", !Equipment.IsTwoHanded(Form.From(sword)));
        check.That("a greatsword is two-handed", Equipment.IsTwoHanded(Form.From(greatsword)));
        check.That("a bow is two-handed", Equipment.IsTwoHanded(Form.From(bow)));

        // Armor weight.
        check.Equal("heavy armor weighs heavy", ArmorWeight.Heavy, Equipment.WeightOf(Form.From(heavy)));
        check.Equal("light armor weighs light", ArmorWeight.Light, Equipment.WeightOf(Form.From(light)));
        check.Equal("clothing has no weight class", ArmorWeight.None, Equipment.WeightOf(Form.From(plain)));

        Native.Backend = null;
    }
}
