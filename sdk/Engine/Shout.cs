using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation;

// One word of a shout: the word of power (a form whose name is the dragon word,
// "Fus"), the spell it casts at that level, and the seconds the Thu'um must
// recover before it can be used again. The three words of a shout grow in power
// and in recovery time.
public readonly struct ShoutWord(Form word, Spell spell, float recoveryTime)
{
    public Form Word { get; } = word;
    public Spell Spell { get; } = spell;
    public float RecoveryTime { get; } = recoveryTime;
}

// A shout (SHOU record): up to three words of power, each a more potent version of
// the Thu'um. Reading the words is the engine's job; learning them, spending dragon
// souls and the recovery cooldown are soft logic (see the Thuum system).
public sealed class Shout : Form
{
    private Shout(ulong handle) : base(handle) { }

    public static new Shout From(ulong handle) => new(handle);
    public static Shout From(Form form) => new(form.Handle);

    // The words this shout is made of, in ascending power, read from its record.
    public IReadOnlyList<ShoutWord> Words => ShoutWordReader.Of(this);
}

// Reads the word entries (SNAM) a shout carries: count then index, the same
// parse-once-then-index ABI the magic-effect reader uses.
internal static class ShoutWordReader
{
    public static IReadOnlyList<ShoutWord> Of(Form shout)
    {
        ulong handle = shout.Handle;
        int count = Native.CallMethod(handle, "GetShoutWordCount", default).AsInt();
        var result = new ShoutWord[count];
        for (int i = 0; i < count; i++)
        {
            Value[] index = { i };
            var word = Form.From(Native.CallMethod(handle, "GetNthShoutWord", index).AsHandle());
            var spell = Spell.From(Native.CallMethod(handle, "GetNthShoutSpell", index).AsHandle());
            float recovery = Native.CallMethod(handle, "GetNthShoutRecoveryTime", index).AsFloat();
            result[i] = new ShoutWord(word, spell, recovery);
        }
        return result;
    }
}
