using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a werewolf takes beast form.
public readonly struct TransformedToBeast(ulong actorHandle) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
}

// Raised when beast form ends, by reverting or by running out of time.
public readonly struct RevertedToHuman(ulong actorHandle) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
}

// Lycanthropy: the beast blood that lets a character take werewolf form for a
// stretch of real time, feeding on the slain to draw it out, until it lapses and
// they revert. This owns the state machine -- who carries the blood, who is
// currently a beast and for how much longer -- and announces every transition;
// the actual change of body is the engine's, made by reacting to the events (a
// race and skeleton swap is not soft logic). Persistent player state; tests clear
// it. The BeastForm system counts the timer down each frame.
public static class Werewolf
{
    // Beast form's base length in real seconds, and the bonus each feeding adds,
    // both as in vanilla. Tunable for a mod that wants a longer hunt.
    public const float BaseFormSeconds = 150f;
    public const float FeedBonusSeconds = 30f;

    private static readonly HashSet<ulong> BeastBlood = new();
    private static readonly Dictionary<ulong, float> Remaining = new();  // beast -> seconds left

    // Grants the beast blood; returns false if the actor already carries it.
    public static bool Bestow(Actor actor) => BeastBlood.Add(actor.Handle);

    public static bool IsWerewolf(Actor actor) => BeastBlood.Contains(actor.Handle);

    public static bool IsTransformed(Actor actor) => Remaining.ContainsKey(actor.Handle);

    public static float RemainingSeconds(Actor actor) => Remaining.GetValueOrDefault(actor.Handle);

    // Takes beast form for `seconds` (the base form by default). Only a werewolf who
    // is not already transformed can. Returns whether it happened.
    public static bool Transform(Actor actor, float seconds = BaseFormSeconds)
    {
        if (!BeastBlood.Contains(actor.Handle) || Remaining.ContainsKey(actor.Handle)) return false;
        Remaining[actor.Handle] = seconds;
        EventBus.Publish(new TransformedToBeast(actor.Handle));
        return true;
    }

    // Feeding on a kill draws out the beast form. A no-op if not transformed.
    public static bool Feed(Actor actor, float bonusSeconds = FeedBonusSeconds)
    {
        if (!Remaining.ContainsKey(actor.Handle)) return false;
        Remaining[actor.Handle] += bonusSeconds;
        return true;
    }

    // Reverts to human now. Returns false if not currently a beast.
    public static bool Revert(Actor actor)
    {
        if (!Remaining.Remove(actor.Handle)) return false;
        EventBus.Publish(new RevertedToHuman(actor.Handle));
        return true;
    }

    // Cures lycanthropy, reverting first if mid-transformation. Returns false if the
    // actor was not a werewolf.
    public static bool Cure(Actor actor)
    {
        if (Remaining.ContainsKey(actor.Handle)) Revert(actor);
        return BeastBlood.Remove(actor.Handle);
    }

    // Counts every active beast form down by `dt` seconds, reverting any that run
    // out. The BeastForm system calls this each frame.
    public static void Tick(float dt)
    {
        if (Remaining.Count == 0) return;
        var keys = new List<ulong>(Remaining.Keys);
        List<ulong>? expired = null;
        foreach (ulong actor in keys)
        {
            float left = Remaining[actor] - dt;
            if (left <= 0f) (expired ??= new List<ulong>()).Add(actor);
            else Remaining[actor] = left;
        }
        if (expired == null) return;
        foreach (ulong actor in expired) Revert(Actor.From(actor));
    }

    public static void Clear()
    {
        BeastBlood.Clear();
        Remaining.Clear();
    }
}

// Counts beast forms down each frame, reverting a werewolf to human when their time
// runs out so the hunt ends on its own.
public sealed class BeastForm : GameBehaviour
{
    protected override void OnUpdate(float deltaTime) => Werewolf.Tick(deltaTime);
}
