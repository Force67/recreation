using System.Collections.Generic;

namespace Recreation;

// A race (RACE record): the species an actor belongs to, carrying its innate
// spells -- the constant-effect abilities (frost resistance, water-breathing) and
// the once-a-day racial power. Reading them is the engine's job; applying the
// abilities (via Abilities) and using the powers (via Powers) is soft logic a mod
// drives, telling the two apart by Spell.Type.
public sealed class Race : Form
{
    private Race(ulong handle) : base(handle) { }

    public static new Race From(ulong handle) => new(handle);
    public static Race From(Form form) => new(form.Handle);

    // The race's innate spells, abilities and powers alike.
    public IReadOnlyList<Spell> Abilities
    {
        get
        {
            int count = Call("GetRaceSpellCount").AsInt();
            var result = new Spell[count];
            for (int i = 0; i < count; i++)
                result[i] = Spell.From(Call("GetNthRaceSpell", i).AsHandle());
            return result;
        }
    }
}
