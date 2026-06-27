using System.Collections.Generic;

namespace Recreation;

// An alchemy ingredient (INGR record): a form carrying up to four magic effects.
// Combining ingredients that share an effect is how potions are brewed; see the
// Skyrim Alchemy system for the rules.
public sealed class Ingredient : Form
{
    private Ingredient(ulong handle) : base(handle) { }

    public static new Ingredient From(ulong handle) => new(handle);

    // The ingredient's magic effects, read from its record. Empty if the form is
    // not an ingredient.
    public IReadOnlyList<MagicEffectInstance> Effects => MagicEffectReader.Of(this);
}
