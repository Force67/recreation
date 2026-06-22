using Recreation.Interop;

namespace Recreation.Games.Starfield;

// Starfield's three damage channels, each met by the spacesuit's matching
// resistance rating; the combat layer (DamageMitigation/WeaponSystems) keys off it.
public enum StarfieldDamageType
{
    Physical,
    Energy,
    Electromagnetic,
}

// A weapon's firing class, from its class keyword. The class sets which damage
// channel a hit lands on and how steeply its damage falls off with range.
public enum StarfieldWeaponClass
{
    None,
    Ballistic,
    Laser,
    Particle,
    Electromagnetic,
}

// A weapon's damage family, from its type keyword. Starfield branches a lot on
// this: EM weapons stun rather than kill, particle beams split damage, and skills
// (Ballistics, Lasers, ...) key off it.
public enum WeaponKind
{
    None,
    Ballistic,
    Laser,
    Particle,
    Electromagnetic,
    Melee,
}

// A piece of apparel's slot, from its keyword. The three armored slots stack their
// resistances in Starfield.
public enum ApparelSlot
{
    None,
    Spacesuit,
    Helmet,
    Pack,
}

// Categorises Starfield gear by the keywords on its record, the counterpart of
// Skyrim's Equipment: a weapon's damage family, an apparel piece's slot, and whether
// a form is ammunition. The keyword ids live in StarfieldForms (placeholders), so
// this is the basis for combat, skill and UI logic that branches on "is this a laser"
// or "is this the spacesuit slot". Pure keyword reads with no state.
public static class StarfieldEquipment
{
    // The weapon's damage family, or None if it carries no weapon-type keyword.
    public static WeaponKind KindOf(Form weapon)
    {
        if (Has(weapon, StarfieldForms.WeaponTypeBallistic)) return WeaponKind.Ballistic;
        if (Has(weapon, StarfieldForms.WeaponTypeLaser)) return WeaponKind.Laser;
        if (Has(weapon, StarfieldForms.WeaponTypeParticle)) return WeaponKind.Particle;
        if (Has(weapon, StarfieldForms.WeaponTypeEM)) return WeaponKind.Electromagnetic;
        if (Has(weapon, StarfieldForms.WeaponTypeMelee)) return WeaponKind.Melee;
        return WeaponKind.None;
    }

    // The apparel slot, or None if the form is not slotted armor (plain clothing).
    public static ApparelSlot SlotOf(Form apparel)
    {
        if (Has(apparel, StarfieldForms.ApparelSpacesuit)) return ApparelSlot.Spacesuit;
        if (Has(apparel, StarfieldForms.ApparelHelmet)) return ApparelSlot.Helmet;
        if (Has(apparel, StarfieldForms.ApparelPack)) return ApparelSlot.Pack;
        return ApparelSlot.None;
    }

    public static bool IsWeapon(Form form) => KindOf(form) != WeaponKind.None;
    public static bool IsApparel(Form form) => SlotOf(form) != ApparelSlot.None;
    public static bool IsAmmo(Form form) => Has(form, StarfieldForms.AmmoKeyword);

    // True for the ranged families that draw from an ammo pool (everything but melee).
    public static bool UsesAmmo(Form weapon) =>
        KindOf(weapon) is WeaponKind.Ballistic or WeaponKind.Laser
            or WeaponKind.Particle or WeaponKind.Electromagnetic;

    private static bool Has(Form form, uint keyword) => form.HasKeyword(Game.GetForm(keyword));

    // --- Combat-channel reads (the basis for WeaponSystems / DamageMitigation) ---

    // The weapon's firing class, or None if it carries no class keyword.
    public static StarfieldWeaponClass ClassOf(WorldForm weapon)
    {
        if (Has(weapon, StarfieldForms.WeaponClassBallistic)) return StarfieldWeaponClass.Ballistic;
        if (Has(weapon, StarfieldForms.WeaponClassLaser)) return StarfieldWeaponClass.Laser;
        if (Has(weapon, StarfieldForms.WeaponClassParticle)) return StarfieldWeaponClass.Particle;
        if (Has(weapon, StarfieldForms.WeaponClassEm)) return StarfieldWeaponClass.Electromagnetic;
        return StarfieldWeaponClass.None;
    }

    // The damage channel a weapon lands on: an explicit damage-type keyword wins,
    // otherwise the channel follows the firing class.
    public static StarfieldDamageType DamageTypeOf(WorldForm weapon)
    {
        if (Has(weapon, StarfieldForms.DamageTypePhysical)) return StarfieldDamageType.Physical;
        if (Has(weapon, StarfieldForms.DamageTypeEnergy)) return StarfieldDamageType.Energy;
        if (Has(weapon, StarfieldForms.DamageTypeElectromagnetic))
            return StarfieldDamageType.Electromagnetic;
        return ChannelFor(ClassOf(weapon));
    }

    // The damage channel a firing class lands on when the record carries no
    // explicit damage-type keyword.
    public static StarfieldDamageType ChannelFor(StarfieldWeaponClass weaponClass) => weaponClass switch
    {
        StarfieldWeaponClass.Laser or StarfieldWeaponClass.Particle => StarfieldDamageType.Energy,
        StarfieldWeaponClass.Electromagnetic => StarfieldDamageType.Electromagnetic,
        _ => StarfieldDamageType.Physical,
    };

    // A WorldForm carries no HasKeyword accessor, so dispatch it raw through the
    // form's own game world, resolving the keyword id in that same world.
    private static bool Has(WorldForm form, uint keyword)
    {
        if (!form.Exists) return false;
        WorldForm kw = form.World.GetForm(keyword);
        return kw.Exists &&
               form.World.CallMethod(form.Handle, "HasKeyword", Value.Object(kw.Handle)).AsBool();
    }
}
