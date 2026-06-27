using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Raised when the character reaches a new level.
public readonly struct CharacterLeveledUp(ulong actorHandle, int level) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public int Level { get; } = level;
}

// Fallout 4 character progression: experience flows in from play, and crossing a
// threshold raises the level and grants one perk point (Fallout 4 spends a point on
// a perk each level, not Skyrim's skills). Each level needs more than the last on a
// tunable ramp, mirroring the game's rising curve. Held in the runtime because the
// engine has no live level for a scripted character. Persistent player state, so it
// outlives the mod-host lifecycle; tests clear it. SpendPerkPoint is what a perk
// pick draws from. Distinct from Starfield's CharacterProgress (separate namespace,
// perks not skill points).
public static class CharacterProgress
{
    public static int StartingLevel { get; set; } = 1;

    // Experience to go from level L to L+1: Base + L * PerLevel (the rising ramp).
    public static float ThresholdBase { get; set; } = 100f;
    public static float ThresholdPerLevel { get; set; } = 75f;

    // Scales every XP gain; an Intelligence bonus (Fallout's "more INT, more XP"
    // rule) or a Well-Rested-style buff can raise it. 1 is the unmodified rate.
    // Session state, reset by Clear.
    public static float XpMultiplier { get; set; } = 1f;

    private readonly record struct Progress(int Level, float Xp, int PerkPoints);
    private static readonly Dictionary<ulong, Progress> State = new();

    private static Progress Of(Actor actor) =>
        State.TryGetValue(actor.Handle, out Progress p) ? p : new Progress(StartingLevel, 0f, 0);

    public static int Level(Actor actor) => Of(actor).Level;
    public static int PerkPoints(Actor actor) => Of(actor).PerkPoints;
    public static float Experience(Actor actor) => Of(actor).Xp;

    public static float Threshold(int level) => ThresholdBase + level * ThresholdPerLevel;

    // Experience still needed to reach the next level.
    public static float XpToNextLevel(Actor actor)
    {
        Progress p = Of(actor);
        return Math.Max(0f, Threshold(p.Level) - p.Xp);
    }

    // Adds experience, leveling up (possibly several times) as thresholds are
    // crossed; each level grants a perk point and raises CharacterLeveledUp.
    public static void GainXp(Actor actor, float xp)
    {
        if (xp <= 0f) return;
        xp *= XpMultiplier;
        Progress p = Of(actor);
        p = p with { Xp = p.Xp + xp };
        while (p.Xp >= Threshold(p.Level))
        {
            float need = Threshold(p.Level);
            p = new Progress(p.Level + 1, p.Xp - need, p.PerkPoints + 1);
            State[actor.Handle] = p;  // keep state current so a handler sees the new level
            EventBus.Publish(new CharacterLeveledUp(actor.Handle, p.Level));
        }
        State[actor.Handle] = p;
    }

    // Spends a perk point if one is available. Returns whether it was spent.
    public static bool SpendPerkPoint(Actor actor)
    {
        Progress p = Of(actor);
        if (p.PerkPoints <= 0) return false;
        State[actor.Handle] = p with { PerkPoints = p.PerkPoints - 1 };
        return true;
    }

    public static void Clear()
    {
        State.Clear();
        XpMultiplier = 1f;
    }
}
