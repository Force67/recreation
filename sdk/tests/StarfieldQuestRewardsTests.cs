using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers quest-driven leveling: each quest/stage milestone pays character XP once
// (a re-fired stage does not farm), and enough progress levels the character and
// grants a skill point. This is the live driver that ties the questing layer to
// the Starfield gameplay loop.
public static class StarfieldQuestRewardsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        int levels = 0;
        using var sub = EventBus.Subscribe<CharacterLeveledUp>(_ => levels++);

        var rewards = new StarfieldQuestRewards { XpPerStage = 100f };
        ModHost.Register(rewards);  // DispatchStart subscribes to QuestStageChanged

        const ulong missionA = 0x500, missionB = 0x600;

        EventBus.Publish(new QuestStageChanged(missionA, 10));
        check.Equal("a stage pays XP", 100f, CharacterProgress.Experience(player));

        // The same milestone re-firing (e.g. a replicated stage) does not pay again.
        EventBus.Publish(new QuestStageChanged(missionA, 10));
        check.Equal("a repeated milestone does not farm XP", 100f, CharacterProgress.Experience(player));

        // The next stage (175 needed for level 2) tips the character over.
        EventBus.Publish(new QuestStageChanged(missionA, 20));
        check.Equal("reached level 2 from quest XP", 2, CharacterProgress.Level(player));
        check.Equal("a skill point was granted", 1, CharacterProgress.SkillPoints(player));
        check.Equal("xp carried past the threshold", 25f, CharacterProgress.Experience(player));
        check.Equal("one level-up event", 1, levels);

        // A different quest's stage is its own milestone.
        EventBus.Publish(new QuestStageChanged(missionB, 10));
        check.Equal("another quest pays its own milestone", 125f, CharacterProgress.Experience(player));

        // Once the behaviour is removed, quest stages no longer pay out.
        ModHost.Unregister(rewards);
        EventBus.Publish(new QuestStageChanged(missionB, 20));
        check.Equal("no XP after the behaviour is removed", 125f, CharacterProgress.Experience(player));

        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
