namespace Recreation.Games.Skyrim;

// A weapon's type (from its WeapType* keyword).
public enum WeaponType
{
    None,
    Dagger,
    Sword,
    WarAxe,
    Mace,
    Greatsword,
    Battleaxe,
    Warhammer,
    Bow,
    Staff,
}

// An armor's weight class (from its ArmorHeavy / ArmorLight keyword).
public enum ArmorWeight
{
    None,
    Light,
    Heavy,
}

// Categorises equipment by the keywords on its record: a weapon's type and an
// armor's weight class. The keyword form ids are Skyrim.esm base-game ids, so this
// is the basis for combat, perk and AI mods that branch on "is this two-handed" or
// "is this heavy armor". Crossbows are a DLC type and are not covered here.
public static class Equipment
{
    private const uint WeapTypeDagger = 0x0001E713;
    private const uint WeapTypeSword = 0x0001E711;
    private const uint WeapTypeWarAxe = 0x0001E712;
    private const uint WeapTypeMace = 0x0001E714;
    private const uint WeapTypeGreatsword = 0x0006D931;
    private const uint WeapTypeBattleaxe = 0x0006D932;
    private const uint WeapTypeWarhammer = 0x0006D930;
    private const uint WeapTypeBow = 0x0001E715;
    private const uint WeapTypeStaff = 0x0001E716;
    private const uint ArmorHeavy = 0x0006BBD2;
    private const uint ArmorLight = 0x0006BBD3;

    // The weapon's type, or None if the form carries no weapon-type keyword.
    public static WeaponType TypeOf(Form weapon)
    {
        if (Has(weapon, WeapTypeDagger)) return WeaponType.Dagger;
        if (Has(weapon, WeapTypeSword)) return WeaponType.Sword;
        if (Has(weapon, WeapTypeWarAxe)) return WeaponType.WarAxe;
        if (Has(weapon, WeapTypeMace)) return WeaponType.Mace;
        if (Has(weapon, WeapTypeGreatsword)) return WeaponType.Greatsword;
        if (Has(weapon, WeapTypeBattleaxe)) return WeaponType.Battleaxe;
        if (Has(weapon, WeapTypeWarhammer)) return WeaponType.Warhammer;
        if (Has(weapon, WeapTypeBow)) return WeaponType.Bow;
        if (Has(weapon, WeapTypeStaff)) return WeaponType.Staff;
        return WeaponType.None;
    }

    // The armor's weight class, or None if it is not weighted armor (clothing).
    public static ArmorWeight WeightOf(Form armor)
    {
        if (Has(armor, ArmorHeavy)) return ArmorWeight.Heavy;
        if (Has(armor, ArmorLight)) return ArmorWeight.Light;
        return ArmorWeight.None;
    }

    // True if the weapon is wielded in both hands.
    public static bool IsTwoHanded(Form weapon) =>
        TypeOf(weapon) is WeaponType.Greatsword or WeaponType.Battleaxe
            or WeaponType.Warhammer or WeaponType.Bow;

    private static bool Has(Form form, uint keyword) => form.HasKeyword(Game.GetForm(keyword));
}
