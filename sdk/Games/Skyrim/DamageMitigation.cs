using System;
using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// Skyrim's damage-mitigation maths: how armor blunts a physical hit and how a
// resistance blunts an elemental or magical one, each with the cap the game
// enforces (armor resists at most 80%, a resistance at most 85%). Pure formulas a
// mod applies when it deals damage; the constants are the vanilla values, tunable
// for a different balance.
public static class DamageMitigation
{
    // Physical: each point of armor resists this percent of a hit, up to the cap.
    public static float ArmorScaling { get; set; } = 0.12f;
    public static float ArmorCapPercent { get; set; } = 80f;

    // Elemental/magical: a resistance is read as a percent and clamped to its cap
    // (the game holds a player's resistances at 85%).
    public static float ResistCapPercent { get; set; } = 85f;

    // Percent of a physical hit that `armorRating` resists, clamped to the armor
    // cap.
    public static float ArmorResistPercent(float armorRating) =>
        Math.Clamp(armorRating * ArmorScaling, 0f, ArmorCapPercent);

    // The physical damage that gets through `armorRating`.
    public static float AfterArmor(float rawDamage, float armorRating) =>
        rawDamage <= 0f ? 0f : rawDamage * (1f - ArmorResistPercent(armorRating) / 100f);

    // The elemental/magical damage that gets through a `resistPercent` resistance,
    // whose effect is clamped to the resistance cap.
    public static float AfterResist(float rawDamage, float resistPercent)
    {
        if (rawDamage <= 0f) return 0f;
        float resisted = Math.Clamp(resistPercent, 0f, ResistCapPercent) / 100f;
        return rawDamage * (1f - resisted);
    }

    // Sums the armor rating of a set of worn pieces, the input to AfterArmor.
    public static float TotalArmorRating(IEnumerable<Form> wornArmor)
    {
        float total = 0f;
        foreach (Form piece in wornArmor) total += piece.ArmorRating;
        return total;
    }
}
