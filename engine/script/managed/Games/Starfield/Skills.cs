using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised when a skill reaches a new rank (1 through 4).
public readonly struct SkillRankUp(ulong actorHandle, string skill, int rank) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public string Skill { get; } = skill;
    public int Rank { get; } = rank;
}

// Starfield skills: a skill has five ranks (0 locked, 1 through 4). Rank 1 is
// bought with a character skill point (Unlock); ranks 2 through 4 are earned by
// completing that rank's usage challenge, no further point needed, the way
// Starfield gates the higher ranks behind "use the skill N times" goals rather
// than spending. Challenge progress and ranks live in the runtime since the
// engine tracks neither for a scripted character. Persistent player state, so it
// outlives the mod-host lifecycle; tests clear it.
public static class Skills
{
    public const int MaxRank = 4;

    // Challenge units to advance FROM rank r to r+1, indexed by r (1..3). Index 0
    // is unused: rank 0 -> 1 is bought with a skill point, not a challenge.
    public static int[] ChallengeTarget { get; set; } = { 0, 10, 25, 50 };

    private static readonly Dictionary<(ulong Actor, string Skill), int> Ranks = new();
    private static readonly Dictionary<(ulong Actor, string Skill), int> Challenge = new();

    public static int Rank(Actor actor, string skill) => Ranks.GetValueOrDefault((actor.Handle, skill));

    // Challenge units banked toward the next rank.
    public static int ChallengeProgress(Actor actor, string skill) =>
        Challenge.GetValueOrDefault((actor.Handle, skill));

    // Buys rank 1 by spending a character skill point. Returns false when the skill
    // is already unlocked or no skill point is available.
    public static bool Unlock(Actor actor, string skill)
    {
        var key = (actor.Handle, skill);
        if (Ranks.GetValueOrDefault(key) >= 1) return false;
        if (!CharacterProgress.SpendSkillPoint(actor)) return false;
        Ranks[key] = 1;
        EventBus.Publish(new SkillRankUp(actor.Handle, skill, 1));
        return true;
    }

    // Advances the current rank's challenge by `amount`, ranking the skill up (1->2
    // ->3->4) as targets are met, no skill point spent. No effect on a locked
    // (rank 0) or maxed (rank 4) skill.
    public static void Progress(Actor actor, string skill, int amount = 1)
    {
        if (amount <= 0) return;
        var key = (actor.Handle, skill);
        int rank = Ranks.GetValueOrDefault(key);
        if (rank < 1 || rank >= MaxRank) return;

        int progress = Challenge.GetValueOrDefault(key) + amount;
        while (rank < MaxRank && progress >= ChallengeTarget[rank])
        {
            progress -= ChallengeTarget[rank];
            rank++;
            Ranks[key] = rank;
            EventBus.Publish(new SkillRankUp(actor.Handle, skill, rank));
        }
        Challenge[key] = rank >= MaxRank ? 0 : progress;
    }

    public static void Clear()
    {
        Ranks.Clear();
        Challenge.Clear();
    }
}
