using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers combat-driven leveling: a kill pays character XP only while the player is
// in combat, the player's own death pays nothing, and enough kills level the
// character and grant a skill point.
public static class StarfieldCombatRewardsTests
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

        var rewards = new StarfieldCombatRewards { XpPerKill = 100f };
        ModHost.Register(rewards);

        const ulong raiderA = 0x800, raiderB = 0x801, raiderC = 0x802;

        // A death while the player is not fighting pays nothing.
        EventBus.Publish(new ActorDied(raiderA));
        check.Equal("a kill outside combat pays no XP", 0f, CharacterProgress.Experience(player));

        // In combat, a kill pays out.
        fake.SetInCombat(player.Handle, true);
        EventBus.Publish(new ActorDied(raiderA));
        check.Equal("a kill in combat pays XP", 100f, CharacterProgress.Experience(player));

        // The player's own death is never a reward.
        EventBus.Publish(new ActorDied(player.Handle));
        check.Equal("the player's death pays nothing", 100f, CharacterProgress.Experience(player));

        // A second kill tips the character to level 2 (175 needed).
        EventBus.Publish(new ActorDied(raiderB));
        check.Equal("reached level 2 from combat XP", 2, CharacterProgress.Level(player));
        check.Equal("a skill point was granted", 1, CharacterProgress.SkillPoints(player));
        check.Equal("one level-up event", 1, levels);

        // Removing the behaviour stops the payouts.
        ModHost.Unregister(rewards);
        float carried = CharacterProgress.Experience(player);
        EventBus.Publish(new ActorDied(raiderC));
        check.Equal("no XP after the behaviour is removed", carried, CharacterProgress.Experience(player));

        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
