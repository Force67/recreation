using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when the player learns a spell from a spell tome.
public readonly struct SpellLearned(ulong readerHandle, ulong spellHandle) : IGameEvent
{
    public ulong ReaderHandle { get; } = readerHandle;
    public ulong SpellHandle { get; } = spellHandle;

    public Actor Reader => Actor.From(ReaderHandle);
    public Form Spell => Form.From(SpellHandle);
}

// Raised when the player gains a skill from a skill book (the first read).
public readonly struct SkillBookRead(ulong bookHandle, string skill) : IGameEvent
{
    public ulong BookHandle { get; } = bookHandle;
    public string Skill { get; } = skill;

    public ObjectReference Book => ObjectReference.From(BookHandle);
}

// Reading books that teach, the Skyrim mechanic: activating a spell tome grants
// its spell and consumes the tome; activating a skill book raises that skill by
// SkillGain the first time. Pure managed soft logic over the activate hook, the
// book's record data and the player's spells and actor values.
public sealed class BookLearning : GameBehaviour
{
    // How much a skill book raises its skill (one in vanilla).
    public int SkillGain { get; set; } = 1;

    private readonly HashSet<ulong> _readSkillBooks = new();
    private EventBus.Subscription? _subscription;

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<PlayerActivated>(OnActivated);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnActivated(PlayerActivated e)
    {
        ObjectReference target = e.Target;
        var book = Recreation.Book.From(target.BaseObject);

        Form spell = book.TeachesSpell;
        if (spell.Exists)
        {
            Game.Player.AddSpell(spell);
            target.Enabled = false;  // the tome is consumed
            EventBus.Publish(new SpellLearned(Game.Player.Handle, spell.Handle));
            return;
        }

        string skill = book.TeachesSkill;
        if (skill.Length > 0 && _readSkillBooks.Add(target.Handle))
        {
            Game.Player.ModValue(skill, SkillGain);
            EventBus.Publish(new SkillBookRead(target.Handle, skill));
        }
    }
}
