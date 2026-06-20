using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers books that teach: the Book wrapper reads what a record teaches, and the
// BookLearning system grants a spell from a tome (consuming it) and raises a
// skill from a skill book once.
public static class BookTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong tomeBase = 0x900, spell = 0xA00, skillBase = 0x901, plainBase = 0x902;
        fake.SetBookSpell(tomeBase, spell);
        fake.SetBookSkill(skillBase, "Destruction");

        // The wrapper reflects what each book teaches.
        check.That("spell tome is recognised", Book.From(tomeBase).IsSpellTome);
        check.Equal("tome teaches its spell", spell, Book.From(tomeBase).TeachesSpell.Handle);
        check.That("skill book is recognised", Book.From(skillBase).IsSkillBook);
        check.Equal("skill book names its skill", "Destruction", Book.From(skillBase).TeachesSkill);
        check.That("a plain book teaches nothing",
                   !Book.From(plainBase).IsSpellTome && !Book.From(plainBase).IsSkillBook);

        ModHost.Register(new BookLearning());

        // Reading a spell tome grants the spell and consumes the tome.
        const ulong tomeRef = 0x100;
        fake.SetBase(tomeRef, tomeBase, essential: false);
        EventBus.Publish(new PlayerActivated(tomeRef));
        check.That("spell tome grants its spell", fake.HasSpell(fake.Player, spell));
        check.That("spell tome is consumed", ObjectReference.From(tomeRef).IsDisabled);

        // Reading a skill book raises the skill, but only the first time.
        const ulong skillRef = 0x101;
        fake.SetBase(skillRef, skillBase, essential: false);
        fake.SetValue(fake.Player, "Destruction", 20, 20);
        EventBus.Publish(new PlayerActivated(skillRef));
        check.Equal("skill book raises the skill", 21f, Game.Player.GetValue("Destruction"));
        EventBus.Publish(new PlayerActivated(skillRef));
        check.Equal("skill book does not raise it again", 21f, Game.Player.GetValue("Destruction"));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
