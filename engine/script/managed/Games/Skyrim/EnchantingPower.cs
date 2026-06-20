namespace Recreation.Games.Skyrim;

// The size of a soul, which caps how much it can power an enchantment.
public enum SoulSize
{
    None = 0,
    Petty = 1,
    Lesser = 2,
    Common = 3,
    Greater = 4,
    Grand = 5,
}

// How strong an enchantment comes out: an effect's base magnitude scaled by the
// enchanter's Enchanting skill and the soul powering it (a bigger soul, a stronger
// enchantment). Pure math with no per-item state, so a mod previews the result at
// an enchanting table or values a soul gem; the curve is tunable. Applying the
// enchantment to a fresh item needs engine support for dynamic forms, so it is not
// part of this -- this is the strength it would have.
public sealed class EnchantingPower
{
    // Magnitude bonus per point of Enchanting skill (the defaults give 1.5x at 100).
    public float SkillBonusPerPoint { get; init; } = 0.005f;

    // The fraction of full strength a soul of each size supplies: a Grand soul
    // gives it all, a Petty soul a fifth.
    public float SoulCharge(SoulSize soul) => (int)soul / 5f;

    // The magnitude an enchantment of `baseMagnitude` applies, scaled by the
    // enchanter's `enchantingSkill` and the `soul` powering it.
    public float Strength(float baseMagnitude, float enchantingSkill, SoulSize soul) =>
        baseMagnitude * (1f + SkillBonusPerPoint * enchantingSkill) * SoulCharge(soul);

    // The strength one of an enchantment's effects would have for `enchanter`.
    public float Strength(MagicEffectInstance effect, Actor enchanter, SoulSize soul) =>
        Strength(effect.Magnitude, enchanter.GetValue(ActorValue.Enchanting), soul);

    // The strength an effect would have powered by the soul in `soulGem`.
    public float Strength(MagicEffectInstance effect, Actor enchanter, SoulGem soulGem) =>
        Strength(effect, enchanter, soulGem.Soul);
}
