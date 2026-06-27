using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// A research project: an id and the skill rank that gates starting it. Completing
// it unlocks the recipes that name it (see Crafting). A managed definition the mod
// or test supplies; the registry only tracks who has finished which.
public readonly struct ResearchProject(string id, string skill, int requiredRank)
{
    public string Id { get; } = id;
    // The skill that gates this research ("" for none) and the rank it needs.
    public string Skill { get; } = skill;
    public int RequiredRank { get; } = requiredRank;
}

// Raised when an actor completes a research project.
public readonly struct ResearchCompleted(ulong actorHandle, string projectId) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public string ProjectId { get; } = projectId;
}

// Starfield research: finishing a project, once its skill-rank gate is met,
// unlocks the recipes that require it. Persistent player state, so it outlives the
// mod-host lifecycle; tests clear it.
public static class Research
{
    private static readonly Dictionary<ulong, HashSet<string>> Done = new();

    public static bool IsComplete(Actor actor, string projectId) =>
        Done.TryGetValue(actor.Handle, out HashSet<string>? set) && set.Contains(projectId);

    // True when the project is not already done and its skill-rank gate is met.
    public static bool CanResearch(Actor actor, ResearchProject project) =>
        !IsComplete(actor, project.Id) &&
        (project.Skill.Length == 0 || Skills.Rank(actor, project.Skill) >= project.RequiredRank);

    // Completes the project when its gate is met. Returns whether it was completed.
    public static bool Complete(Actor actor, ResearchProject project)
    {
        if (!CanResearch(actor, project)) return false;
        if (!Done.TryGetValue(actor.Handle, out HashSet<string>? set))
            Done[actor.Handle] = set = new HashSet<string>();
        set.Add(project.Id);
        EventBus.Publish(new ResearchCompleted(actor.Handle, project.Id));
        return true;
    }

    public static void Clear() => Done.Clear();
}
