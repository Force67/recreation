using System.Collections.Generic;

namespace Recreation;

// One of an ingredient's magic effects, with the base magnitude and duration its
// record carries (before any alchemy-skill scaling is applied at brew time).
public readonly struct IngredientEffect(MagicEffect effect, float magnitude, int duration)
{
    public MagicEffect Effect { get; } = effect;
    public float Magnitude { get; } = magnitude;
    public int Duration { get; } = duration;
}

// An alchemy ingredient (INGR record): a form carrying up to four magic effects.
// Combining ingredients that share an effect is how potions are brewed; see the
// Skyrim Alchemy system for the rules.
public sealed class Ingredient : Form
{
    private Ingredient(ulong handle) : base(handle) { }

    public static new Ingredient From(ulong handle) => new(handle);

    // The ingredient's magic effects, read from its record. Materialised eagerly
    // because the engine caches the parse between the count and the per-index
    // reads. Empty if the form is not an ingredient.
    public IReadOnlyList<IngredientEffect> Effects
    {
        get
        {
            int count = Call("GetIngredientEffectCount").AsInt();
            var result = new IngredientEffect[count];
            for (int i = 0; i < count; i++)
            {
                var effect = MagicEffect.From(Call("GetNthIngredientEffectId", i).AsHandle());
                float magnitude = Call("GetNthIngredientEffectMagnitude", i).AsFloat();
                int duration = Call("GetNthIngredientEffectDuration", i).AsInt();
                result[i] = new IngredientEffect(effect, magnitude, duration);
            }
            return result;
        }
    }
}
