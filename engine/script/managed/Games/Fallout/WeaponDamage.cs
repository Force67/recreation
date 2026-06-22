using System;
using System.Collections.Generic;

namespace Recreation.Games.Fallout;

// Fallout 4 weapon damage: a weapon's base damage scaled by the wielder's
// SPECIAL- and perk-driven multipliers, paired with the channel it lands on so
// DamageResistance meets it with the right resistance. Melee and unarmed damage
// rises with Strength (the vanilla "Melee Damage" derived stat, +10% per point),
// and the relevant weapon perk (Rifleman, Commando, ...) multiplies a ranged hit
// per rank. Pure math with no per-weapon state, so a mod previews a hit or totals
// a loadout; the curves are tunable. The damage type is read from the weapon's
// keywords the way Skyrim's Equipment reads its weapon-type keyword.
public static class WeaponDamage
{
    // Vanilla Melee Damage bonus: each Strength point above 1 adds this fraction
    // of a melee or unarmed weapon's base damage.
    public static float MeleeDamagePerStrength { get; set; } = 0.10f;

    // Each rank of the matching ranged-weapon perk adds this fraction (the
    // Rifleman/Commando line is +20% per rank in vanilla).
    public static float PerkDamagePerRank { get; set; } = 0.20f;

    // The melee/unarmed damage multiplier at `strength` (SPECIAL 1..10).
    public static float MeleeMultiplier(float strength) =>
        1f + MeleeDamagePerStrength * Math.Max(0f, strength - 1f);

    // The ranged damage multiplier from `perkRank` ranks of the weapon's perk.
    public static float RangedMultiplier(int perkRank) =>
        1f + PerkDamagePerRank * Math.Max(0, perkRank);

    // A melee hit's damage: base scaled by Strength.
    public static float MeleeDamage(float baseDamage, float strength) =>
        baseDamage <= 0f ? 0f : baseDamage * MeleeMultiplier(strength);

    // A ranged hit's damage: base scaled by the weapon perk's rank.
    public static float RangedDamage(float baseDamage, int perkRank) =>
        baseDamage <= 0f ? 0f : baseDamage * RangedMultiplier(perkRank);

    // The damage type a weapon deals, read from its channel keyword. Defaults to
    // Physical when the weapon carries no damage-type keyword (the common
    // ballistic case).
    public static DamageType TypeOf(Form weapon)
    {
        if (Has(weapon, FalloutForms.DamageTypeEnergy)) return DamageType.Energy;
        if (Has(weapon, FalloutForms.DamageTypeRadiation)) return DamageType.Radiation;
        return DamageType.Physical;
    }

    // A melee weapon's damage for `wielder`, reading its base damage off the
    // record and the wielder's Strength.
    public static float MeleeDamage(Form weapon, Actor wielder) =>
        MeleeDamage(weapon.WeaponDamage, wielder.GetValue(FalloutActorValue.Strength));

    private static bool Has(Form form, uint keyword) => form.HasKeyword(Game.GetForm(keyword));
}
