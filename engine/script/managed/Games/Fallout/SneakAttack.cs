using System;

namespace Recreation.Games.Fallout;

// Fallout 4 sneak-attack and VATS-critical damage maths: the deterministic
// multipliers stacked onto a hit that lands undetected or as a VATS crit. A
// sneak attack does double damage in vanilla, ranged Ninja ranks push it to
// 2.5x and melee Ninja ranks to a 10x finisher; a VATS critical adds the full
// base damage again (a guaranteed +100%), raised +50% per Better Criticals rank.
// VATS itself is gated on Action Points, so a crit is only available when the
// attacker can afford the queued shot. Pure math given the inputs, so a mod
// resolves the damage of a planned strike; the multipliers are the vanilla
// values, tunable.
public static class SneakAttack
{
    // Base sneak-attack multiplier (vanilla: a sneak attack does double damage).
    public static float SneakMultiplier { get; set; } = 2f;

    // Ranged Ninja: each rank adds this to the ranged sneak multiplier (rank 3
    // reaches the vanilla 2.5x).
    public static float NinjaRangedPerRank { get; set; } = 0.5f / 3f;

    // Melee Ninja: each rank multiplies the melee sneak bonus toward the 10x
    // finisher (vanilla rank 3 turns the 2x into 10x, i.e. x5 on top).
    public static float NinjaMeleePerRank { get; set; } = 4f / 3f;

    // A VATS critical deals an extra fraction of base damage; Better Criticals
    // raises it +50% per rank.
    public static float CritBonusFraction { get; set; } = 1f;
    public static float BetterCriticalsPerRank { get; set; } = 0.5f;

    // The AP a single VATS attack reserves; a crit needs at least this much to
    // queue. The default mirrors a light weapon's per-shot cost.
    public static float VatsApCost { get; set; } = 25f;

    // The ranged sneak-attack multiplier at `ninjaRank` (0 = none).
    public static float RangedSneakMultiplier(int ninjaRank) =>
        SneakMultiplier + NinjaRangedPerRank * Math.Max(0, ninjaRank);

    // The melee sneak-attack multiplier at `ninjaRank`: Ninja scales the base
    // bonus, so rank 0 is the plain 2x and the curve climbs toward the finisher.
    public static float MeleeSneakMultiplier(int ninjaRank) =>
        SneakMultiplier * (1f + NinjaMeleePerRank * Math.Max(0, ninjaRank));

    // The VATS-critical multiplier at `betterCriticalsRank` (0 = none): a crit
    // adds the base damage again, raised per rank.
    public static float CritMultiplier(int betterCriticalsRank) =>
        1f + CritBonusFraction * (1f + BetterCriticalsPerRank * Math.Max(0, betterCriticalsRank));

    // The damage of a ranged sneak attack: base scaled by the sneak multiplier.
    public static float RangedSneakDamage(float baseDamage, int ninjaRank = 0) =>
        baseDamage <= 0f ? 0f : baseDamage * RangedSneakMultiplier(ninjaRank);

    // The damage of a melee sneak attack.
    public static float MeleeSneakDamage(float baseDamage, int ninjaRank = 0) =>
        baseDamage <= 0f ? 0f : baseDamage * MeleeSneakMultiplier(ninjaRank);

    // The damage of a VATS critical hit.
    public static float CritDamage(float baseDamage, int betterCriticalsRank = 0) =>
        baseDamage <= 0f ? 0f : baseDamage * CritMultiplier(betterCriticalsRank);

    // Whether `attacker` can queue a VATS crit: they must hold the AP a shot
    // reserves. Reads ActionPoints off the attacker's actor values.
    public static bool CanVatsCrit(Actor attacker) =>
        attacker.GetValue(FalloutActorValue.ActionPoints) >= VatsApCost;
}
