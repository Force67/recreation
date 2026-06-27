using System;
using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a trainer raises a student's skill a point.
public readonly struct SkillTrained(ulong studentHandle, string skill, int level, int cost) : IGameEvent
{
    public ulong StudentHandle { get; } = studentHandle;
    public string Skill { get; } = skill;
    public int Level { get; } = level;
    public int Cost { get; } = cost;
}

// Trainers: paying gold to raise a skill a point, capped at five lessons per
// character level the way Skyrim limits it. The cost climbs with the skill, and a
// lesson advances the character toward the next level just as using the skill
// would, so a paid-for skill-up can itself reset the allowance. Soft logic over the
// inventory, the skill progression and the character level. Persistent player
// state, so it outlives the mod-host lifecycle; tests clear it.
public static class Training
{
    public static int MaxPerLevel { get; set; } = 5;

    // Gold to train a skill from level L: Base + PerLevel * L (a rising price).
    public static float CostBase { get; set; } = 10f;
    public static float CostPerLevel { get; set; } = 10f;

    // Lessons taken at the character level they were taken at, so a new level
    // refreshes the allowance.
    private static readonly Dictionary<ulong, (int Level, int Used)> Sessions = new();
    private static Form GoldForm => Game.GetForm(SkyrimForms.Gold);

    public static int Cost(Actor student, string skill) =>
        (int)(CostBase + CostPerLevel * student.GetBaseValue(skill));

    // Lessons still available this character level.
    public static int LessonsLeft(Actor student)
    {
        (int Level, int Used) s = Sessions.GetValueOrDefault(student.Handle);
        return s.Level == CharacterLevel.Level(student) ? Math.Max(0, MaxPerLevel - s.Used) : MaxPerLevel;
    }

    // Trains `skill` one point: charges the student gold and raises the skill, if
    // they can afford it, have a lesson left this level, and the skill is not capped.
    // Returns the gold spent, or 0 if no lesson was given.
    public static int Train(Actor student, string skill)
    {
        if (LessonsLeft(student) <= 0) return 0;
        if (student.GetBaseValue(skill) >= SkillProgression.MaxSkill) return 0;
        int cost = Cost(student, skill);
        if (student.GetItemCount(GoldForm) < cost) return 0;

        student.RemoveItem(GoldForm, cost);
        int level = SkillProgression.Increment(student, skill);

        int charLevel = CharacterLevel.Level(student);
        (int Level, int Used) s = Sessions.GetValueOrDefault(student.Handle);
        Sessions[student.Handle] = s.Level == charLevel ? (charLevel, s.Used + 1) : (charLevel, 1);
        EventBus.Publish(new SkillTrained(student.Handle, skill, level, cost));
        return cost;
    }

    public static void Clear() => Sessions.Clear();
}
