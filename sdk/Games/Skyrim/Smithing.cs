namespace Recreation.Games.Skyrim;

// Tempering: improving a weapon's damage or an armor's rating at a workbench, the
// gain scaling with the smith's Smithing skill and doubled by the matching perk.
// Pure math with no per-item state, so a mod previews the tempered result or
// totals a fully-tempered loadout, and the curve is tunable by constructing one
// with different factors.
public sealed class Smithing
{
    // Smithing skill needed for each quality tier (Fine, Superior, Exquisite, ...).
    public float SkillPerTier { get; init; } = 20f;

    // Fraction of the base value each tier adds.
    public float ImprovementPerTier { get; init; } = 0.25f;

    // The quality tiers reachable at `smithingSkill`; the matching perk doubles it.
    public int Tiers(float smithingSkill, bool hasPerk = false)
    {
        int tiers = smithingSkill <= 0f ? 0 : (int)(smithingSkill / SkillPerTier);
        return hasPerk ? tiers * 2 : tiers;
    }

    // The tempered weapon damage: each tier adds a fraction of the base.
    public int TemperedDamage(int baseDamage, float smithingSkill, bool hasPerk = false) =>
        baseDamage + (int)(baseDamage * ImprovementPerTier * Tiers(smithingSkill, hasPerk));

    // The tempered armor rating.
    public float TemperedArmor(float baseArmor, float smithingSkill, bool hasPerk = false) =>
        baseArmor * (1f + ImprovementPerTier * Tiers(smithingSkill, hasPerk));

    // Tempers a weapon for `smith` at their Smithing skill.
    public int TemperedDamage(Form weapon, Actor smith, bool hasPerk = false) =>
        TemperedDamage(weapon.WeaponDamage, smith.GetValue(ActorValue.Smithing), hasPerk);

    // Tempers a piece of armor for `smith` at their Smithing skill.
    public float TemperedArmor(Form armor, Actor smith, bool hasPerk = false) =>
        TemperedArmor(armor.ArmorRating, smith.GetValue(ActorValue.Smithing), hasPerk);
}
