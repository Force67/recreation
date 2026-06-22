namespace Recreation.Games.Starfield;

// Well-known Starfield.esm form ids the gameplay layer refers to by name instead
// of a bare hex literal. These are base-game ids (load order 00); resolve them at
// runtime with Starfield.GetForm.
//
// PLACEHOLDER ids: unlike SkyrimForms these were not probed from the .esm, so the
// hex values are stand-ins chosen only to be distinct. Replace them with the real
// records once dumped; the systems reference the names, so a fix is one edit here.
public static class StarfieldForms
{
    // The keyword marking an item as breathable air the player can top off from
    // (an O2 canister or aid item). OxygenCo2 refills oxygen when one is consumed,
    // the way SurvivalNeeds feeds on the food keyword. PLACEHOLDER.
    public const uint OxygenSourceKeyword = 0x0000A101;

    // The Settled Systems' major factions, the ones the player builds standing
    // with (FactionReputation) and answers to for crimes (Bounties). Distinct
    // stand-in ids; the systems refer to them by name, so a fix is one edit here.
    // PLACEHOLDER.
    public const uint UnitedColoniesFaction = 0x0000A201;
    public const uint FreestarCollectiveFaction = 0x0000A202;
    public const uint ConstellationFaction = 0x0000A203;
    public const uint CrimsonFleetFaction = 0x0000A204;
    public const uint RyujinIndustriesFaction = 0x0000A205;

    // Equipment-category keywords Starfield stamps on its gear, the basis for
    // StarfieldEquipment's weapon/ammo/spacesuit classification (the counterpart of
    // Skyrim's WeapType*/Armor* keywords). PLACEHOLDER.
    public const uint WeaponTypeBallistic = 0x0000A301;
    public const uint WeaponTypeLaser = 0x0000A302;
    public const uint WeaponTypeParticle = 0x0000A303;
    public const uint WeaponTypeEM = 0x0000A304;
    public const uint WeaponTypeMelee = 0x0000A305;
    public const uint ApparelSpacesuit = 0x0000A311;
    public const uint ApparelHelmet = 0x0000A312;
    public const uint ApparelPack = 0x0000A313;
    public const uint AmmoKeyword = 0x0000A321;

    // Damage-channel keywords on a weapon's record: Starfield splits a hit across
    // Physical / Energy / Electromagnetic, each met by the suit's matching
    // resistance. WeaponSystems and DamageMitigation read the channel from these.
    // PLACEHOLDER.
    public const uint DamageTypePhysical = 0x0000A331;
    public const uint DamageTypeEnergy = 0x0000A332;
    public const uint DamageTypeElectromagnetic = 0x0000A333;
    // Weapon firing-class keywords (distinct from the WeaponType* skill keywords
    // above): the class sets a weapon's falloff feel and damage channel. PLACEHOLDER.
    public const uint WeaponClassBallistic = 0x0000A341;
    public const uint WeaponClassLaser = 0x0000A342;
    public const uint WeaponClassParticle = 0x0000A343;
    public const uint WeaponClassEm = 0x0000A344;
}
