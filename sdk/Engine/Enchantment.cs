using System.Collections.Generic;

namespace Recreation;

// An enchantment (ENCH record): the set of magic effects an enchanted weapon or
// armor carries. Like a potion, it lists its effects as record data, so the same
// accessor reads them. What an enchantment does is data; disenchanting to learn it
// is soft logic (see the Skyrim Enchanting service).
public sealed class Enchantment : Form
{
    private Enchantment(ulong handle) : base(handle) { }

    public static new Enchantment From(ulong handle) => new(handle);
    public static Enchantment From(Form form) => new(form.Handle);

    // The magic effects this enchantment applies, read from its record.
    public IReadOnlyList<MagicEffectInstance> Effects => MagicEffectReader.Of(this);
}
