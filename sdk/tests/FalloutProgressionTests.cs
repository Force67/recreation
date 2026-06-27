using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Fallout 4 progression: experience levels the character and grants one perk
// point per level, the threshold ramps up each level, XP banks under the next
// threshold, and a perk point is spent only when one is available.
public static class FalloutProgressionTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        CharacterProgress.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        int levels = 0;
        using var sub = EventBus.Subscribe<CharacterLeveledUp>(_ => levels++);

        // Level 1 -> 2 needs Threshold(1) = 100 + 75 = 175 xp.
        check.Equal("xp to next level at start", 175f, CharacterProgress.XpToNextLevel(player));
        CharacterProgress.GainXp(player, 175f);
        check.Equal("reached level 2", 2, CharacterProgress.Level(player));
        check.Equal("granted a perk point", 1, CharacterProgress.PerkPoints(player));
        check.Equal("one level-up event", 1, levels);

        CharacterProgress.GainXp(player, 100f);  // banked toward level 3 (needs 250)
        check.Equal("xp banked under the next threshold", 100f, CharacterProgress.Experience(player));
        check.Equal("xp to next level", 150f, CharacterProgress.XpToNextLevel(player));

        // Spend the perk point.
        check.That("spend a perk point", CharacterProgress.SpendPerkPoint(player));
        check.Equal("the point was spent", 0, CharacterProgress.PerkPoints(player));
        check.That("cannot spend a point you do not have", !CharacterProgress.SpendPerkPoint(player));

        // A big award levels several times at once, each granting a point.
        CharacterProgress.GainXp(player, 1000f);  // crosses several thresholds
        check.That("multiple levels gained", CharacterProgress.Level(player) >= 4);
        check.That("perk points accrued per level", CharacterProgress.PerkPoints(player) >= 2);

        // A multiplier scales the gain (the INT bonus feeds this): 50 counts as 100,
        // banked under the level-2 threshold of 175.
        CharacterProgress.Clear();
        CharacterProgress.XpMultiplier = 2f;
        CharacterProgress.GainXp(player, 50f);  // counts as 100
        check.Equal("multiplier scales xp", 100f, CharacterProgress.Experience(player));
        check.Equal("multiplier did not level up", 1, CharacterProgress.Level(player));

        CharacterProgress.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
