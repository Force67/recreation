using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Thu'um: a shout is learned word by word, dragon souls unlock each word
// for use, shouting uses the highest unlocked level and triggers a recovery that
// counts down before the next shout.
public static class ThuumTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        Thuum.Clear();
        EventBus.Clear();

        // Unrelenting Force: three words of rising power and recovery.
        const ulong shoutId = 0xF00;
        fake.SetShoutWords(shoutId,
            (word: 0xFA0, spell: 0xFB0, recovery: 15f),
            (word: 0xFA1, spell: 0xFB1, recovery: 20f),
            (word: 0xFA2, spell: 0xFB2, recovery: 45f));
        var shout = Shout.From(shoutId);
        Actor player = Game.Player;

        // The record reads back as three ascending words.
        check.Equal("the shout has three words", 3, shout.Words.Count);
        check.Equal("first word's recovery", 15f, shout.Words[0].RecoveryTime);
        check.Equal("third word's recovery", 45f, shout.Words[2].RecoveryTime);
        check.Equal("first word's spell", 0xFB0UL, shout.Words[0].Spell.Handle);

        int learned = 0, used = 0;
        using var s1 = EventBus.Subscribe<ShoutWordLearned>(_ => learned++);
        using var s2 = EventBus.Subscribe<ShoutUsed>(_ => used++);

        // Unknown and unusable up front.
        check.Equal("no words learned", 0, Thuum.LearnedWords(player, shout));
        check.That("cannot shout an unknown shout", !Thuum.CanShout(player, shout));
        check.That("using it does nothing", !Thuum.Use(player, shout).Shouted);

        // Learn all three at word walls; a fourth has nothing to learn.
        check.That("learn the first word", Thuum.LearnWord(player, shout));
        Thuum.LearnWord(player, shout);
        Thuum.LearnWord(player, shout);
        check.Equal("three words learned", 3, Thuum.LearnedWords(player, shout));
        check.That("nothing left to learn", !Thuum.LearnWord(player, shout));
        check.Equal("a learn event per word", 3, learned);

        // Learning is not unlocking: it still cannot be shouted without a soul.
        check.Equal("nothing unlocked yet", 0, Thuum.UnlockedWords(player, shout));
        check.That("no soul, no unlock", !Thuum.UnlockWord(player, shout));

        // Dragon souls unlock words for use, one soul each.
        Thuum.GainDragonSoul(player, 2);
        check.That("first unlock spends a soul", Thuum.UnlockWord(player, shout));
        check.That("second unlock spends the other", Thuum.UnlockWord(player, shout));
        check.Equal("two words unlocked", 2, Thuum.UnlockedWords(player, shout));
        check.That("out of souls, no third unlock", !Thuum.UnlockWord(player, shout));
        check.Equal("souls spent", 0, Thuum.DragonSouls(player));

        // Shouting uses the highest unlocked level and starts its recovery.
        check.That("now it can shout", Thuum.CanShout(player, shout));
        ShoutResult result = Thuum.Use(player, shout);
        check.That("the shout went off", result.Shouted);
        check.Equal("at the second level", 2, result.Level);
        check.Equal("delivering the level-two spell", 0xFB1UL, result.Spell.Handle);
        check.Equal("recovery started", 20f, Thuum.Cooldown(player));
        check.Equal("one shout event", 1, used);

        // Recovering: it cannot shout again until the cooldown drains.
        check.That("cannot shout while recovering", !Thuum.CanShout(player, shout));
        Thuum.Tick(10f);
        check.Equal("recovery ticks down", 10f, Thuum.Cooldown(player));
        Thuum.Tick(15f);  // past zero
        check.Equal("recovery is done", 0f, Thuum.Cooldown(player));
        check.That("ready to shout again", Thuum.CanShout(player, shout));

        Thuum.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
