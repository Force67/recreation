using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers character leveling: experience banks toward the next level, crossing the
// threshold (possibly several at once) raises the level and grants perk points, and
// points can be spent.
public static class CharacterLevelTests
{
    public static void Run(Check check)
    {
        CharacterLevel.Clear();
        EventBus.Clear();
        CharacterLevel.StartingLevel = 1;
        CharacterLevel.ThresholdBase = 100f;     // a flat 100 per level for clean maths
        CharacterLevel.ThresholdPerLevel = 0f;

        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        Actor player = Game.Player;

        int ups = 0;
        using var sub = EventBus.Subscribe<LeveledUp>(_ => ups++);

        check.Equal("starts at the starting level", 1, CharacterLevel.Level(player));
        check.Equal("no perk points yet", 0, CharacterLevel.PerkPoints(player));
        check.Equal("a full threshold to go", 100f, CharacterLevel.XpToNextLevel(player));

        CharacterLevel.GainXp(player, 60f);
        check.Equal("xp banked, no level yet", 1, CharacterLevel.Level(player));
        check.Equal("threshold remaining", 40f, CharacterLevel.XpToNextLevel(player));

        CharacterLevel.GainXp(player, 50f);  // 110 crosses into level two, carrying 10
        check.Equal("crossed into level two", 2, CharacterLevel.Level(player));
        check.Equal("a perk point granted", 1, CharacterLevel.PerkPoints(player));
        check.Equal("the carry counts toward the next", 90f, CharacterLevel.XpToNextLevel(player));
        check.Equal("one level-up event", 1, ups);

        CharacterLevel.GainXp(player, 190f);  // 10+190=200 spans two levels at once
        check.Equal("multiple levels in one gain", 4, CharacterLevel.Level(player));
        check.Equal("perk points accrue per level", 3, CharacterLevel.PerkPoints(player));
        check.Equal("a level-up event per level", 3, ups);

        check.That("a perk point can be spent", CharacterLevel.SpendPerkPoint(player));
        check.Equal("spending decrements them", 2, CharacterLevel.PerkPoints(player));

        CharacterLevel.ThresholdBase = 75f;  // restore defaults for other tests
        CharacterLevel.ThresholdPerLevel = 25f;
        CharacterLevel.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
