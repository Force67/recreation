namespace Recreation.Games.Skyrim;

// A soul gem (SLGM record): a vessel of a given capacity (its size class) that
// holds a soul of some size. Reading its soul state is the engine's job; using a
// trapped soul to power an enchantment or recharge a weapon is soft logic (see
// EnchantingPower).
public sealed class SoulGem : Form
{
    private SoulGem(ulong handle) : base(handle) { }

    public static new SoulGem From(ulong handle) => new(handle);
    public static SoulGem From(Form form) => new(form.Handle);

    // The soul currently held, or None if the gem is empty.
    public SoulSize Soul => (SoulSize)Call("GetSoulGemSoul").AsInt();

    // The largest soul the gem can hold (its size class).
    public SoulSize Capacity => (SoulSize)Call("GetSoulGemCapacity").AsInt();

    // True if the gem holds a soul.
    public bool IsFilled => Soul != SoulSize.None;

    // True if the held soul fills the gem to its capacity.
    public bool IsFull => Capacity != SoulSize.None && Soul == Capacity;
}
