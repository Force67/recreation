using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers skill progression: using a skill banks experience, crossing the threshold
// raises the skill a point and feeds the character's level, and a skill stops at
// its cap.
public static class SkillProgressionTests
{
    public static void Run(Check check)
    {
        SkillProgression.Clear();
        CharacterLevel.Clear();
        EventBus.Clear();

        // Clean curves: a point costs 10 x skill level; each point is worth its new
        // level in character xp; the character needs a flat 10 per level.
        SkillProgression.UseMult = 10f;
        SkillProgression.UseExponent = 1f;
        SkillProgression.UseOffset = 0f;
        SkillProgression.CharXpPerSkillLevel = 1f;
        SkillProgression.MaxSkill = 100f;
        CharacterLevel.StartingLevel = 1;
        CharacterLevel.ThresholdBase = 10f;
        CharacterLevel.ThresholdPerLevel = 0f;

        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.OneHanded, current: 15, baseValue: 15);

        int skillUps = 0, levelUps = 0;
        using var s1 = EventBus.Subscribe<SkillIncreased>(_ => skillUps++);
        using var s2 = EventBus.Subscribe<LeveledUp>(_ => levelUps++);

        // Below the threshold (10 x 15 = 150): banks progress, no point.
        SkillProgression.Gain(player, ActorValue.OneHanded, 50f);
        check.Equal("progress banked under the threshold", 50f,
                    SkillProgression.ProgressToNext(player, ActorValue.OneHanded));
        check.Equal("skill unchanged", 15f, player.GetBaseValue(ActorValue.OneHanded));

        // Crossing it raises the skill and feeds the character level.
        SkillProgression.Gain(player, ActorValue.OneHanded, 100f);  // pool 150 = threshold(15)
        check.Equal("the skill rose a point", 16f, player.GetBaseValue(ActorValue.OneHanded));
        check.Equal("one skill-up fired", 1, skillUps);
        check.Equal("progress reset after the point", 0f,
                    SkillProgression.ProgressToNext(player, ActorValue.OneHanded));
        // The point was worth 16 character xp; a flat-10 ramp levels the character.
        check.Equal("the skill-up leveled the character", 2, CharacterLevel.Level(player));
        check.That("the level-up fired", levelUps >= 1);
        check.That("a perk point came with it", CharacterLevel.PerkPoints(player) >= 1);

        // A skill cannot rise past its cap, however much is poured in.
        fake.SetValue(player.Handle, ActorValue.Destruction, current: 99, baseValue: 99);
        SkillProgression.Gain(player, ActorValue.Destruction, 100000f);
        check.Equal("the skill stops at the cap", 100f, player.GetBaseValue(ActorValue.Destruction));

        // Restore defaults for other tests.
        SkillProgression.UseMult = 1f;
        SkillProgression.UseExponent = 1.95f;
        SkillProgression.CharXpPerSkillLevel = 1f;
        CharacterLevel.ThresholdBase = 75f;
        CharacterLevel.ThresholdPerLevel = 25f;
        SkillProgression.Clear();
        CharacterLevel.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
