using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation;

// A magic effect (MGEF record): the atom a spell, enchantment, potion or
// ingredient applies (Restore Health, Damage Stamina, Fortify Smithing, ...).
// Used as an identity to match the effects two ingredients share, and (for
// consumables and spells) to read which actor value it changes and in which
// direction.
public sealed class MagicEffect : Form
{
    private MagicEffect(ulong handle) : base(handle) { }

    public static new MagicEffect From(ulong handle) => new(handle);

    // The actor-value name this effect modifies (e.g. "Health"), or "" for an
    // effect the actor-value store does not model (light, paralysis, ...).
    public string ActorValue => Call("GetMagicEffectActorValue").AsString();

    // True if the effect damages rather than restores or fortifies its value.
    public bool IsDetrimental => Call("GetMagicEffectDetrimental").AsBool();

    // The effect's base cost (MGEF DATA): the unit price that scales the magicka
    // cost of a spell carrying it and the gold value of a potion brewed with it.
    public float BaseCost => Call("GetMagicEffectBaseCost").AsFloat();
}

// One magic effect carried by an ingredient, potion, enchantment or spell: the
// effect plus the base magnitude and duration the record gives it. A duration of
// 0 is an instant effect; a positive duration is a timed one.
public readonly struct MagicEffectInstance(MagicEffect effect, float magnitude, int duration)
{
    public MagicEffect Effect { get; } = effect;
    public float Magnitude { get; } = magnitude;
    public int Duration { get; } = duration;

    // Applies this effect to `target`: an instant effect restores or damages the
    // value at once; a timed one fortifies or saps it for its duration through the
    // Effects scheduler. Returns false (changing nothing) for an effect on a value
    // the engine does not model.
    public bool ApplyTo(Actor target)
    {
        string actorValue = Effect.ActorValue;
        if (actorValue.Length == 0) return false;
        bool harmful = Effect.IsDetrimental;
        if (Duration <= 0)
        {
            if (harmful) target.DamageValue(actorValue, Magnitude);
            else target.RestoreValue(actorValue, Magnitude);
        }
        else
        {
            Modding.Effects.Apply(target, actorValue, harmful ? -Magnitude : Magnitude, Duration);
        }
        return true;
    }
}

// Reads the magic effects a form carries (ingredient, potion, enchantment, spell).
// One place for the count-then-index walk so each wrapper's Effects property stays
// a one-liner.
internal static class MagicEffectReader
{
    public static IReadOnlyList<MagicEffectInstance> Of(Form form)
    {
        ulong handle = form.Handle;
        int count = Native.CallMethod(handle, "GetMagicEffectCount", default).AsInt();
        var result = new MagicEffectInstance[count];
        for (int i = 0; i < count; i++)
        {
            Value[] index = { i };
            var effect = MagicEffect.From(Native.CallMethod(handle, "GetNthMagicEffectId", index).AsHandle());
            float magnitude = Native.CallMethod(handle, "GetNthMagicEffectMagnitude", index).AsFloat();
            int duration = Native.CallMethod(handle, "GetNthMagicEffectDuration", index).AsInt();
            result[i] = new MagicEffectInstance(effect, magnitude, duration);
        }
        return result;
    }
}
