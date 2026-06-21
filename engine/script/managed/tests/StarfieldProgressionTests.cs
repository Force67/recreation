using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield progression: experience levels the character and grants skill
// points, a point unlocks a skill's first rank, and the higher ranks are earned
// by completing each rank's usage challenge rather than spending more points.
public static class StarfieldProgressionTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        CharacterProgress.Clear();
        Skills.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        int levels = 0, rankUps = 0;
        using var s1 = EventBus.Subscribe<CharacterLeveledUp>(_ => levels++);
        using var s2 = EventBus.Subscribe<SkillRankUp>(_ => rankUps++);

        // Level 1 -> 2 needs Threshold(1) = 100 + 75 = 175 xp.
        CharacterProgress.GainXp(player, 175f);
        check.Equal("reached level 2", 2, CharacterProgress.Level(player));
        check.Equal("granted a skill point", 1, CharacterProgress.SkillPoints(player));
        check.Equal("one level-up event", 1, levels);

        CharacterProgress.GainXp(player, 100f);  // banked toward level 3 (needs 250)
        check.Equal("xp banked under the next threshold", 100f, CharacterProgress.Experience(player));
        check.Equal("xp to next level", 150f, CharacterProgress.XpToNextLevel(player));

        // Spend the point to unlock a skill's first rank.
        check.That("unlock a skill with a point", Skills.Unlock(player, "BoostPackTraining"));
        check.Equal("skill at rank 1", 1, Skills.Rank(player, "BoostPackTraining"));
        check.Equal("the point was spent", 0, CharacterProgress.SkillPoints(player));
        check.That("cannot re-unlock the same skill", !Skills.Unlock(player, "BoostPackTraining"));
        check.That("cannot unlock another without a point", !Skills.Unlock(player, "Ballistics"));

        // A locked skill ignores challenge progress.
        Skills.Progress(player, "Ballistics", 100);
        check.Equal("locked skill stays rank 0", 0, Skills.Rank(player, "Ballistics"));

        // Challenges carry ranks 1->2->3->4 (targets 10, 25, 50), no point spent.
        Skills.Progress(player, "BoostPackTraining", 10);
        check.Equal("challenge ranks to 2", 2, Skills.Rank(player, "BoostPackTraining"));
        Skills.Progress(player, "BoostPackTraining", 25);
        check.Equal("challenge ranks to 3", 3, Skills.Rank(player, "BoostPackTraining"));
        Skills.Progress(player, "BoostPackTraining", 50);
        check.Equal("challenge ranks to 4", 4, Skills.Rank(player, "BoostPackTraining"));
        Skills.Progress(player, "BoostPackTraining", 100);
        check.Equal("rank 4 is the cap", 4, Skills.Rank(player, "BoostPackTraining"));

        // Earn another point and watch partial challenge progress bank.
        CharacterProgress.GainXp(player, 150f);  // 100 + 150 = 250 -> level 3, +1 point
        check.Equal("reached level 3", 3, CharacterProgress.Level(player));
        check.That("unlock a second skill", Skills.Unlock(player, "Ballistics"));
        Skills.Progress(player, "Ballistics", 5);  // target is 10
        check.Equal("partial challenge stays rank 1", 1, Skills.Rank(player, "Ballistics"));
        check.Equal("challenge progress banked", 5, Skills.ChallengeProgress(player, "Ballistics"));
        Skills.Progress(player, "Ballistics", 5);  // reaches 10 -> rank up
        check.Equal("challenge completes to rank 2", 2, Skills.Rank(player, "Ballistics"));

        check.Equal("two character levels gained", 2, levels);
        check.Equal("six rank-up events fired", 6, rankUps);

        CharacterProgress.Clear();
        Skills.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
