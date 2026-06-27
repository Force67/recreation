using System.Collections.Generic;

namespace Recreation;

// A potion, food or poison (ALCH record): an item that applies its magic effects
// when consumed. Reading the effects is the engine's job; applying them is soft
// logic here, built on the actor-value calls and the Effects scheduler so it
// works with no engine support beyond the record data.
public sealed class Potion : Form
{
    private Potion(ulong handle) : base(handle) { }

    public static new Potion From(ulong handle) => new(handle);
    public static Potion From(Form form) => new(form.Handle);

    // The effects this consumable carries, read from its record.
    public IReadOnlyList<MagicEffectInstance> Effects => MagicEffectReader.Of(this);

    // Applies every effect to `drinker`, skipping those on values the engine does
    // not model. Returns how many were applied.
    public int Consume(Actor drinker)
    {
        int applied = 0;
        foreach (MagicEffectInstance e in Effects)
            if (e.ApplyTo(drinker)) applied++;
        return applied;
    }
}
