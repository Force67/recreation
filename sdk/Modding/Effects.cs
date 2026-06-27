using System.Collections.Generic;
using System.Linq;

namespace Recreation.Modding;

// One active timed modifier on an actor value.
public sealed class Effect
{
    public Actor Target { get; }
    public string ActorValue { get; }
    public float Magnitude { get; }
    public bool Active { get; internal set; } = true;

    internal ScheduledTask? Expiry;

    internal Effect(Actor target, string actorValue, float magnitude)
    {
        Target = target;
        ActorValue = actorValue;
        Magnitude = magnitude;
    }
}

// Raised when a timed effect is applied to an actor.
public readonly struct EffectApplied(Effect effect) : IGameEvent
{
    public Effect Effect { get; } = effect;
}

// Raised when a timed effect ends (expired or removed).
public readonly struct EffectExpired(Effect effect) : IGameEvent
{
    public Effect Effect { get; } = effect;
}

// Applies temporary modifiers to actor values, the managed analog of Skyrim's
// magic effects: a potion, enchantment or shout adds a magnitude for a duration
// and the value is restored when it runs out. It is built entirely in managed
// code on actor values and the scheduler, so any mod can grant timed buffs
// without engine support. Because the magnitude is applied and later subtracted
// relatively (ModValue), effects stack and unwind cleanly.
public static class Effects
{
    private static readonly List<Effect> Active = new();

    // Applies magnitude to actorValue on target for durationSeconds (a duration
    // of 0 or less lasts until removed). Returns a handle to query or end early.
    public static Effect Apply(Actor target, string actorValue, float magnitude,
                               float durationSeconds)
    {
        var effect = new Effect(target, actorValue, magnitude);
        target.ModValue(actorValue, magnitude);
        Active.Add(effect);
        if (durationSeconds > 0f)
            effect.Expiry = Scheduler.After(durationSeconds, () => Remove(effect));
        EventBus.Publish(new EffectApplied(effect));
        return effect;
    }

    // Ends an effect early, restoring the modified value. Safe to call twice.
    public static void Remove(Effect effect)
    {
        if (!effect.Active) return;
        effect.Active = false;
        effect.Expiry?.Cancel();
        effect.Target.ModValue(effect.ActorValue, -effect.Magnitude);
        Active.Remove(effect);
        EventBus.Publish(new EffectExpired(effect));
    }

    public static int ActiveCount => Active.Count;

    public static IEnumerable<Effect> ActiveOn(Actor target) =>
        Active.Where(e => e.Target.Handle == target.Handle);

    // Ends every active effect, restoring values. Used on managed-world teardown.
    public static void Clear()
    {
        foreach (Effect effect in Active.ToArray()) Remove(effect);
    }
}
