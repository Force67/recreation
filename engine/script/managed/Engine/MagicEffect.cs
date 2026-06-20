namespace Recreation;

// A magic effect (MGEF record): the atom a spell, enchantment, potion or
// ingredient applies (Restore Health, Damage Stamina, Fortify Smithing, ...).
// Mod code mostly uses it as an identity, to match the effects two ingredients
// share when brewing; richer MGEF data can hang off this as the engine exposes it.
public sealed class MagicEffect : Form
{
    private MagicEffect(ulong handle) : base(handle) { }

    public static new MagicEffect From(ulong handle) => new(handle);
}
