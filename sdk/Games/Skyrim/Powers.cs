using System;
using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// Greater powers: a spell used once, then unavailable until game time passes (a
// day in vanilla). Use casts the power if it is ready and stamps the game time;
// Ready and Cooldown query it against the clock. The cooldown is measured in game
// days, the unit powers recharge in, so it is unaffected by how fast real time
// runs. Self-contained (it only remembers when each power was used), so it needs
// no teardown hook; tests clear it.
public static class Powers
{
    // Game days before a used power is ready again (one in vanilla). A mod sets it
    // shorter for a lesser power or longer for a daily that spans more.
    public static float CooldownDays { get; set; } = 1f;

    private static readonly Dictionary<ulong, float> UsedAt = new();  // power -> game time used

    public static bool Ready(Spell power) =>
        !UsedAt.TryGetValue(power.Handle, out float used) ||
        GameClock.GameTime - used >= CooldownDays;

    // Game days remaining until the power recharges (0 if it is ready now).
    public static float Cooldown(Spell power) =>
        UsedAt.TryGetValue(power.Handle, out float used)
            ? MathF.Max(0f, CooldownDays - (GameClock.GameTime - used))
            : 0f;

    // Uses a self-targeted power (the common case) on `caster` if it is ready.
    public static bool Use(Actor caster, Spell power) => Use(caster, caster, power);

    // Uses a power at `target` if it is ready: casts it and starts the cooldown.
    // Returns whether it fired.
    public static bool Use(Actor caster, Actor target, Spell power)
    {
        if (!Ready(power)) return false;
        if (!power.Cast(caster, target)) return false;
        UsedAt[power.Handle] = GameClock.GameTime;
        return true;
    }

    public static void Clear() => UsedAt.Clear();
}
