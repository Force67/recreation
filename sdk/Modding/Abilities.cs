using System.Collections.Generic;

namespace Recreation.Modding;

// Passive abilities: a spell's effects applied to an actor for as long as it holds
// the ability, the constant-effect counterpart to a one-shot cast. Racial powers,
// standing-stone blessings and item-set bonuses are abilities. Apply fortifies the
// actor's values through the Effects scheduler as permanent modifiers (no expiry);
// Remove reverts them as a unit. Apply is idempotent per (actor, ability), so a
// blessing cannot stack with itself.
//
// This differs from casting a spell or drinking a potion, which apply each effect
// once: an ability holds its fortify for as long as it is active.
public static class Abilities
{
    private static readonly Dictionary<(ulong Actor, ulong Ability), List<Effect>> Active = new();

    // Grants `ability` to `actor`, fortifying each of its values. Returns false if
    // the actor already has it.
    public static bool Apply(Actor actor, Spell ability)
    {
        var key = (actor.Handle, ability.Handle);
        if (Active.ContainsKey(key)) return false;

        var effects = new List<Effect>();
        foreach (MagicEffectInstance e in ability.Effects)
        {
            string actorValue = e.Effect.ActorValue;
            if (actorValue.Length == 0) continue;  // a value the engine does not model
            float magnitude = e.Effect.IsDetrimental ? -e.Magnitude : e.Magnitude;
            effects.Add(Effects.Apply(actor, actorValue, magnitude, durationSeconds: 0f));
        }
        Active[key] = effects;
        return true;
    }

    // Removes `ability` from `actor`, reverting its modifiers. Returns false if the
    // actor did not have it.
    public static bool Remove(Actor actor, Spell ability)
    {
        if (!Active.Remove((actor.Handle, ability.Handle), out List<Effect>? effects)) return false;
        foreach (Effect effect in effects) Effects.Remove(effect);
        return true;
    }

    public static bool IsActive(Actor actor, Spell ability) =>
        Active.ContainsKey((actor.Handle, ability.Handle));

    public static int ActiveCount => Active.Count;

    // Drops all ability bookkeeping. The underlying modifiers are cleared by
    // Effects.Clear in the same teardown.
    public static void Clear() => Active.Clear();
}
