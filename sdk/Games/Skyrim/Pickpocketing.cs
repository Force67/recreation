using System;

namespace Recreation.Games.Skyrim;

// The odds of pickpocketing an item: the Skyrim shape, where a higher Pickpocket
// skill helps and a heavier or more valuable item (and taking more of it) hurts,
// clamped to a floor and a ceiling so nothing is ever certain. The exact constants
// are tunable so a mod can set its own curve; the defaults give the vanilla feel
// (near the cap at 100 skill on a light trinket, slim odds at low skill on a heavy
// purse).
public static class PickpocketOdds
{
    // A flat starting chance before skill, the per-skill-point gain, and the
    // penalties per unit of an item's weight and gold value.
    public static int BaseChance { get; set; } = 10;
    public static float SkillFactor { get; set; } = 0.8f;
    public static float WeightPenalty { get; set; } = 3f;
    public static float ValuePenalty { get; set; } = 0.1f;

    // No pickpocket is a sure thing, nor wholly hopeless: chances clamp here.
    public static int MaxChance { get; set; } = 90;
    public static int MinChance { get; set; } = 0;

    // Percent chance in [MinChance, MaxChance] to lift `count` of `item` at the
    // given Pickpocket skill.
    public static int Chance(float pickpocketSkill, Form item, int count = 1)
    {
        float raw = BaseChance + SkillFactor * pickpocketSkill
                    - count * (WeightPenalty * item.Weight + ValuePenalty * item.GoldValue);
        return (int)Math.Clamp(raw, MinChance, MaxChance);
    }
}

// The outcome of a pickpocket attempt: whether there was anything to take, whether
// it succeeded, and the chance it was resolved against (handy for UI).
public readonly struct PickpocketResult(bool attempted, bool success, int chance)
{
    public bool Attempted { get; } = attempted;
    public bool Success { get; } = success;
    public int Chance { get; } = chance;

    // A failed attempt is a detected one: the caller reports the crime.
    public bool Caught => Attempted && !Success;
}

// Pickpocketing: lifting an item from another actor against the odds. Soft logic
// over the inventory and the odds curve; the caller supplies a roll in [0, 100) so
// the outcome is deterministic and the RNG stays the mod's choice. On success the
// items move to the thief; on failure nothing moves and the theft is detected
// (PickpocketResult.Caught), leaving the crime report to the caller, who knows the
// victim's hold.
public static class Pickpocket
{
    public static PickpocketResult Attempt(Actor thief, Actor victim, Form item, int roll, int count = 1)
    {
        int have = victim.GetItemCount(item);
        if (have <= 0 || count <= 0) return new PickpocketResult(attempted: false, success: false, chance: 0);
        count = Math.Min(count, have);

        int chance = PickpocketOdds.Chance(thief.GetValue(ActorValue.Pickpocket), item, count);
        bool success = roll < chance;
        if (success) victim.TransferItemTo(thief, item, count);
        return new PickpocketResult(attempted: true, success: success, chance: chance);
    }
}
