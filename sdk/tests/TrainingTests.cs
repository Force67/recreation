using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers trainers: a lesson costs gold that climbs with the skill, raises the skill
// a point, is capped at five per character level, and the cap refreshes when the
// character levels.
public static class TrainingTests
{
    public static void Run(Check check)
    {
        Training.Clear();
        SkillProgression.Clear();
        CharacterLevel.Clear();
        EventBus.Clear();

        Training.MaxPerLevel = 5;
        Training.CostBase = 10f;
        Training.CostPerLevel = 10f;
        SkillProgression.MaxSkill = 100f;
        SkillProgression.CharXpPerSkillLevel = 1f;
        CharacterLevel.StartingLevel = 1;
        CharacterLevel.ThresholdBase = 100000f;  // keep training from auto-leveling mid-test
        CharacterLevel.ThresholdPerLevel = 0f;

        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.OneHanded, current: 20, baseValue: 20);
        Form gold = Game.GetForm(SkyrimForms.Gold);
        player.AddItem(gold, 2000);

        int trained = 0;
        using var sub = EventBus.Subscribe<SkillTrained>(_ => trained++);

        check.Equal("cost climbs with the skill", 210, Training.Cost(player, ActorValue.OneHanded));
        check.Equal("five lessons to start", 5, Training.LessonsLeft(player));

        int spent = Training.Train(player, ActorValue.OneHanded);
        check.Equal("charged the lesson cost", 210, spent);
        check.Equal("the skill rose a point", 21f, player.GetBaseValue(ActorValue.OneHanded));
        check.Equal("gold was deducted", 1790, player.GetItemCount(gold));
        check.Equal("a lesson was used", 4, Training.LessonsLeft(player));
        check.Equal("a trained event fired", 1, trained);

        // Take the remaining four; the sixth is refused even with gold to spare.
        Training.Train(player, ActorValue.OneHanded);  // 21 -> 22
        Training.Train(player, ActorValue.OneHanded);  // 22 -> 23
        Training.Train(player, ActorValue.OneHanded);  // 23 -> 24
        Training.Train(player, ActorValue.OneHanded);  // 24 -> 25
        check.Equal("the allowance is spent", 0, Training.LessonsLeft(player));
        check.Equal("the sixth lesson is refused", 0, Training.Train(player, ActorValue.OneHanded));
        check.Equal("skill unchanged by the refusal", 25f, player.GetBaseValue(ActorValue.OneHanded));

        // Leveling the character refreshes the allowance.
        CharacterLevel.GainXp(player, 200000f);  // forces at least one level
        check.That("the character leveled", CharacterLevel.Level(player) > 1);
        check.Equal("a new level refreshes lessons", 5, Training.LessonsLeft(player));

        // Without the gold, no lesson is given.
        fake.SetValue(player.Handle, ActorValue.Sneak, current: 90, baseValue: 90);
        player.RemoveItem(gold, player.GetItemCount(gold));  // empty the purse
        check.Equal("a pauper cannot train", 0, Training.Train(player, ActorValue.Sneak));
        check.Equal("the skill stayed put", 90f, player.GetBaseValue(ActorValue.Sneak));

        Training.CostBase = 10f;  // restore defaults for other tests
        Training.CostPerLevel = 10f;
        CharacterLevel.ThresholdBase = 75f;
        CharacterLevel.ThresholdPerLevel = 25f;
        Training.Clear();
        SkillProgression.Clear();
        CharacterLevel.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
