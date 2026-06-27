using System;
using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a vampire's stage changes: the stage left and the stage entered.
// Stage 0 means "not a vampire", so 0 -> 1 is the turning and N -> 0 is a cure.
// Mods apply each stage's powers and weaknesses by reacting to this.
public readonly struct VampireStageChanged(ulong actorHandle, int previous, int stage) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public int Previous { get; } = previous;
    public int Stage { get; } = stage;
}

// Vampirism progression: a vampire grows hungrier each day they do not feed,
// advancing through four stages (well-fed to starved) that the game makes ever
// more powerful and ever more sun-sick. Feeding resets the hunger to stage one.
// This owns the timing rule -- stage from the game clock since the last feeding --
// and announces every transition; the stage's actual powers are a mod's to grant
// off the event, the gmod-style split between rule and content. Persistent player
// state; tests clear it. The VampireProgression system advances it each frame.
public static class Vampirism
{
    // The starved final stage, and how many game days of hunger advance each stage
    // (one in vanilla).
    public const int MaxStage = 4;
    public static float DaysPerStage { get; set; } = 1f;

    private static readonly Dictionary<ulong, float> FedAt = new();    // actor -> game time fed
    private static readonly Dictionary<ulong, int> Reported = new();   // actor -> announced stage

    public static bool IsVampire(Actor actor) => FedAt.ContainsKey(actor.Handle);

    // Turns `actor` into a vampire at the well-fed first stage. Returns false if
    // they already are one.
    public static bool Infect(Actor actor)
    {
        if (FedAt.ContainsKey(actor.Handle)) return false;
        FedAt[actor.Handle] = GameClock.GameTime;
        Reported[actor.Handle] = 0;
        Refresh();  // announces 0 -> 1
        return true;
    }

    // Feeding resets the hunger clock, dropping the vampire back to stage one.
    public static void Feed(Actor actor)
    {
        if (!FedAt.ContainsKey(actor.Handle)) return;
        FedAt[actor.Handle] = GameClock.GameTime;
        Refresh();  // announces a drop back to stage one
    }

    // The vampire's current stage (1..4), or 0 if `actor` is not a vampire.
    public static int StageOf(Actor actor)
    {
        if (!FedAt.TryGetValue(actor.Handle, out float fed)) return 0;
        int stage = 1 + (int)MathF.Floor((GameClock.GameTime - fed) / DaysPerStage);
        return Math.Clamp(stage, 1, MaxStage);
    }

    public static float DaysSinceFed(Actor actor) =>
        FedAt.TryGetValue(actor.Handle, out float fed) ? GameClock.GameTime - fed : 0f;

    // Cures `actor` of vampirism. Returns false if they were not a vampire.
    public static bool Cure(Actor actor)
    {
        if (!FedAt.Remove(actor.Handle)) return false;
        int previous = Reported.GetValueOrDefault(actor.Handle);
        Reported.Remove(actor.Handle);
        EventBus.Publish(new VampireStageChanged(actor.Handle, previous, 0));
        return true;
    }

    // Recomputes every vampire's stage and announces the ones that moved. The
    // progression system calls this each frame so hunger advances on its own.
    public static void Refresh()
    {
        List<ulong>? movers = null;
        foreach (ulong actor in FedAt.Keys)
        {
            int now = 1 + (int)MathF.Floor((GameClock.GameTime - FedAt[actor]) / DaysPerStage);
            now = Math.Clamp(now, 1, MaxStage);
            if (Reported.GetValueOrDefault(actor) != now) (movers ??= new List<ulong>()).Add(actor);
        }
        if (movers == null) return;
        foreach (ulong actor in movers)
        {
            int previous = Reported.GetValueOrDefault(actor);
            int now = StageOf(Actor.From(actor));
            Reported[actor] = now;
            EventBus.Publish(new VampireStageChanged(actor, previous, now));
        }
    }

    public static void Clear()
    {
        FedAt.Clear();
        Reported.Clear();
    }
}

// Advances vampirism each frame: it lets the hunger clock move vampires to their
// next stage on its own, so a player who does not feed grows more powerful and
// more sun-sick without anything prompting it.
public sealed class VampireProgression : GameBehaviour
{
    protected override void OnUpdate(float deltaTime) => Vampirism.Refresh();
}
