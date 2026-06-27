namespace Recreation;

// A book (BOOK record). Beyond being readable, a book may teach: a spell tome
// grants a spell, a skill book raises a skill by one on first read. Reading the
// record is the engine's job; what reading does is soft logic a mod drives (see
// the Skyrim BookLearning system).
public sealed class Book : Form
{
    private Book(ulong handle) : base(handle) { }

    public static new Book From(ulong handle) => new(handle);
    public static Book From(Form form) => new(form.Handle);

    // The spell a spell tome teaches, or Form.None if this book teaches no spell.
    public Form TeachesSpell => Form.From(Call("GetBookSpell").AsHandle());

    // The actor-value name of the skill a skill book raises, or "" if none.
    public string TeachesSkill => Call("GetBookSkill").AsString();

    public bool IsSpellTome => TeachesSpell.Exists;
    public bool IsSkillBook => TeachesSkill.Length > 0;
}
