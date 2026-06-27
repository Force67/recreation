using System;
using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a word of a shout is learned at a word wall.
public readonly struct ShoutWordLearned(ulong actorHandle, ulong shoutHandle, int words) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public ulong ShoutHandle { get; } = shoutHandle;
    public int Words { get; } = words;  // how many words of the shout are now known
}

// Raised when a shout is used, carrying the power level it was used at.
public readonly struct ShoutUsed(ulong actorHandle, ulong shoutHandle, int level) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public ulong ShoutHandle { get; } = shoutHandle;
    public int Level { get; } = level;
}

// The outcome of a shout: whether it went off, the spell to deliver (the engine
// aims and casts it), the power level used, and the recovery it triggered.
public readonly struct ShoutResult(bool shouted, Spell spell, int level, float recovery)
{
    public bool Shouted { get; } = shouted;
    public Spell Spell { get; } = spell;
    public int Level { get; } = level;
    public float Recovery { get; } = recovery;

    public static ShoutResult None { get; } = new(false, Spell.From(0), 0, 0f);
}

// The Thu'um: learning shouts word by word at word walls, spending dragon souls to
// unlock each word for use, and the shared recovery between shouts. This owns that
// progression and the cooldown; delivering the shout's spell (aiming the projectile
// or area) is the engine's, done with the Spell the result hands back. Persistent
// player state, so it outlives the mod-host lifecycle; tests clear it. The
// ShoutCooldown system counts recovery down each frame.
public static class Thuum
{
    private readonly record struct Knowledge(int Learned, int Unlocked);

    private static readonly Dictionary<(ulong Actor, ulong Shout), Knowledge> Words = new();
    private static readonly Dictionary<ulong, int> Souls = new();        // actor -> dragon souls
    private static readonly Dictionary<ulong, float> Recovery = new();   // actor -> cooldown seconds

    public static int LearnedWords(Actor actor, Shout shout) =>
        Words.GetValueOrDefault((actor.Handle, shout.Handle)).Learned;

    public static int UnlockedWords(Actor actor, Shout shout) =>
        Words.GetValueOrDefault((actor.Handle, shout.Handle)).Unlocked;

    // Learns the next word of `shout` at a word wall. Returns false if every word is
    // already known.
    public static bool LearnWord(Actor actor, Shout shout)
    {
        var key = (actor.Handle, shout.Handle);
        Knowledge k = Words.GetValueOrDefault(key);
        if (k.Learned >= shout.Words.Count) return false;
        k = k with { Learned = k.Learned + 1 };
        Words[key] = k;
        EventBus.Publish(new ShoutWordLearned(actor.Handle, shout.Handle, k.Learned));
        return true;
    }

    public static int DragonSouls(Actor actor) => Souls.GetValueOrDefault(actor.Handle);

    public static void GainDragonSoul(Actor actor, int count = 1) =>
        Souls[actor.Handle] = Math.Max(0, DragonSouls(actor) + count);

    // Spends a dragon soul to unlock the next learned-but-locked word of `shout` for
    // use. Returns false if there is none to unlock or no soul to spend.
    public static bool UnlockWord(Actor actor, Shout shout)
    {
        var key = (actor.Handle, shout.Handle);
        Knowledge k = Words.GetValueOrDefault(key);
        if (k.Unlocked >= k.Learned || DragonSouls(actor) <= 0) return false;
        Souls[actor.Handle] = DragonSouls(actor) - 1;
        Words[key] = k with { Unlocked = k.Unlocked + 1 };
        return true;
    }

    // Seconds before the next shout can be used.
    public static float Cooldown(Actor actor) => Recovery.GetValueOrDefault(actor.Handle);

    public static bool CanShout(Actor actor, Shout shout) =>
        UnlockedWords(actor, shout) > 0 && Cooldown(actor) <= 0f;

    // Uses `shout` at the highest unlocked power level: hands back the spell to
    // deliver and starts the shared recovery. A no-op (ShoutResult.None) if no word
    // is unlocked or the Thu'um is still recovering.
    public static ShoutResult Use(Actor actor, Shout shout)
    {
        if (!CanShout(actor, shout)) return ShoutResult.None;
        int level = UnlockedWords(actor, shout);
        ShoutWord word = shout.Words[level - 1];
        Recovery[actor.Handle] = word.RecoveryTime;
        EventBus.Publish(new ShoutUsed(actor.Handle, shout.Handle, level));
        return new ShoutResult(true, word.Spell, level, word.RecoveryTime);
    }

    // Counts every actor's recovery down by `dt` seconds. The ShoutCooldown system
    // calls this each frame.
    public static void Tick(float dt)
    {
        if (Recovery.Count == 0) return;
        var keys = new List<ulong>(Recovery.Keys);
        foreach (ulong actor in keys)
        {
            float left = Recovery[actor] - dt;
            if (left <= 0f) Recovery.Remove(actor);
            else Recovery[actor] = left;
        }
    }

    public static void Clear()
    {
        Words.Clear();
        Souls.Clear();
        Recovery.Clear();
    }
}

// Counts shout recovery down each frame so the Thu'um becomes ready again on its
// own.
public sealed class ShoutCooldown : GameBehaviour
{
    protected override void OnUpdate(float deltaTime) => Thuum.Tick(deltaTime);
}
