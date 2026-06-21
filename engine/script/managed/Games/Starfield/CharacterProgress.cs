using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised when the character reaches a new level.
public readonly struct CharacterLeveledUp(ulong actorHandle, int level) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public int Level { get; } = level;
}

// Starfield character progression: experience flows in from play, and crossing a
// threshold raises the level and grants one skill point (Starfield spends points
// on skills, not Skyrim's perks). Each level needs more than the last on a
// tunable ramp. Held in the runtime because the engine has no live level for a
// scripted character. Persistent player state, so it outlives the mod-host
// lifecycle; tests clear it. Skills.Unlock spends the points this grants.
public static class CharacterProgress
{
    public static int StartingLevel { get; set; } = 1;

    // Experience to go from level L to L+1: Base + L * PerLevel (the rising ramp).
    public static float ThresholdBase { get; set; } = 100f;
    public static float ThresholdPerLevel { get; set; } = 75f;

    private readonly record struct Progress(int Level, float Xp, int SkillPoints);
    private static readonly Dictionary<ulong, Progress> State = new();

    private static Progress Of(Actor actor) =>
        State.TryGetValue(actor.Handle, out Progress p) ? p : new Progress(StartingLevel, 0f, 0);

    public static int Level(Actor actor) => Of(actor).Level;
    public static int SkillPoints(Actor actor) => Of(actor).SkillPoints;
    public static float Experience(Actor actor) => Of(actor).Xp;

    public static float Threshold(int level) => ThresholdBase + level * ThresholdPerLevel;

    // Experience still needed to reach the next level.
    public static float XpToNextLevel(Actor actor)
    {
        Progress p = Of(actor);
        return Math.Max(0f, Threshold(p.Level) - p.Xp);
    }

    // Adds experience, leveling up (possibly several times) as thresholds are
    // crossed; each level grants a skill point and raises CharacterLeveledUp.
    public static void GainXp(Actor actor, float xp)
    {
        if (xp <= 0f) return;
        Progress p = Of(actor);
        p = p with { Xp = p.Xp + xp };
        while (p.Xp >= Threshold(p.Level))
        {
            float need = Threshold(p.Level);
            p = new Progress(p.Level + 1, p.Xp - need, p.SkillPoints + 1);
            State[actor.Handle] = p;  // keep state current so a handler sees the new level
            EventBus.Publish(new CharacterLeveledUp(actor.Handle, p.Level));
        }
        State[actor.Handle] = p;
    }

    // Spends a skill point if one is available. Returns whether it was spent.
    public static bool SpendSkillPoint(Actor actor)
    {
        Progress p = Of(actor);
        if (p.SkillPoints <= 0) return false;
        State[actor.Handle] = p with { SkillPoints = p.SkillPoints - 1 };
        return true;
    }

    public static void Clear() => State.Clear();
}
