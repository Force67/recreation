using System;
using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a skill rises by a point, carrying its new level.
public readonly struct SkillIncreased(ulong actorHandle, string skill, int level) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public string Skill { get; } = skill;
    public int Level { get; } = level;
}

// Skill progression: using a skill earns experience toward its next point, and
// every point earned feeds the character's level. The engine holds the skill value
// (raised here through the actor); the use-experience toward the next point lives
// in the runtime. A point costs more the higher the skill, on a tunable,
// vanilla-shaped curve, and a skill stops at its cap. Persistent player state, so
// it outlives the mod-host lifecycle; tests clear it.
public static class SkillProgression
{
    // Experience for the next point at skill level L: Mult * L^Exponent + Offset.
    public static float UseMult { get; set; } = 1.0f;
    public static float UseExponent { get; set; } = 1.95f;
    public static float UseOffset { get; set; } = 0f;

    // Character experience each point is worth (scaled by the new skill level), and
    // the level a skill cannot rise past.
    public static float CharXpPerSkillLevel { get; set; } = 1.0f;
    public static float MaxSkill { get; set; } = 100f;

    private static readonly Dictionary<(ulong Actor, string Skill), float> Pool = new();

    public static float Threshold(float skillLevel) =>
        UseMult * MathF.Pow(MathF.Max(skillLevel, 1f), UseExponent) + UseOffset;

    // The use-experience banked toward `skill`'s next point.
    public static float ProgressToNext(Actor actor, string skill) =>
        Pool.GetValueOrDefault((actor.Handle, skill));

    // Adds use-experience to `skill`, raising it a point at a time as thresholds are
    // crossed (until the cap) and feeding the character's level for each point.
    public static void Gain(Actor actor, string skill, float xp)
    {
        if (xp <= 0f) return;
        var key = (actor.Handle, skill);
        float pool = Pool.GetValueOrDefault(key) + xp;
        float level = actor.GetBaseValue(skill);
        while (level < MaxSkill && pool >= Threshold(level))
        {
            pool -= Threshold(level);
            level += 1f;
            actor.SetValue(skill, level);
            EventBus.Publish(new SkillIncreased(actor.Handle, skill, (int)level));
            CharacterLevel.GainXp(actor, CharXpPerSkillLevel * level);
        }
        Pool[key] = pool;
    }

    public static void Clear() => Pool.Clear();
}
