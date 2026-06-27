using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Survival-mode Adrenaline: kills in combat build ranks (a damage bonus per
// rank) up to a cap, kills outside combat and the player's own death earn nothing,
// and idle time out of combat decays a rank while a sustained fight holds it.
public static class FalloutAdrenalineTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        Actor player = Game.Player;

        int changes = 0;
        using var sub = EventBus.Subscribe<AdrenalineChanged>(_ => changes++);

        var adrenaline = new Adrenaline
        {
            KillsPerRank = 2,
            MaxRank = 3,
            DamagePerRank = 0.1f,
            DecaySeconds = 10f,
        };
        ModHost.Register(adrenaline);
        check.Equal("starts at rank 0", 0, adrenaline.Rank);

        const ulong raider = 0x800;

        // Kills outside combat earn nothing.
        EventBus.Publish(new ActorDied(raider));
        check.Equal("no rank without combat", 0, adrenaline.Rank);

        // In combat, two kills earn one rank.
        fake.SetInCombat(player.Handle, true);
        EventBus.Publish(new ActorDied(raider));
        check.Equal("partial progress holds at rank 0", 0, adrenaline.Rank);
        EventBus.Publish(new ActorDied(raider));
        check.Equal("two kills earn a rank", 1, adrenaline.Rank);
        check.Equal("the damage bonus follows the rank", 0.1f, adrenaline.DamageBonus);

        // The player's own death never builds adrenaline.
        EventBus.Publish(new ActorDied(player.Handle));
        check.Equal("the player's death earns nothing", 1, adrenaline.Rank);

        // A sustained fight does not decay the meter even past the decay window.
        ModHost.Tick(30f);
        check.Equal("combat holds the rank", 1, adrenaline.Rank);

        // Climb to the cap and confirm it holds there.
        for (int i = 0; i < 8; i++) EventBus.Publish(new ActorDied(raider));
        check.Equal("ranks cap at MaxRank", 3, adrenaline.Rank);

        // Standing down: idle time decays a rank once the window passes.
        fake.SetInCombat(player.Handle, false);
        ModHost.Tick(10f);
        check.Equal("idle time decays a rank", 2, adrenaline.Rank);
        ModHost.Tick(5f);  // under the window
        check.Equal("partial idle does not decay", 2, adrenaline.Rank);

        check.That("rank changes were announced", changes > 0);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
