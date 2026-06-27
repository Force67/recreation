using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers sleep recovery and the Well Rested bonus: resting tops the breath meters
// back up and boosts XP gains for a span of game-time, and the bonus winds back to
// the normal rate once that span elapses.
public static class WellRestedTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { GameTime = 10.0f };
        Native.Backend = fake;
        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        var oxygen = new OxygenCo2 { HypoxiaAfterSeconds = 1000f, DrainPerSecond = 25f };
        var rested = new WellRested { Bonus = 0.5f, DurationGameHours = 24f };
        ModHost.Register(oxygen);
        ModHost.Register(rested);

        // Spend some oxygen so a rest has something to restore.
        oxygen.Exerting = true;
        ModHost.Tick(2f);
        check.That("oxygen was spent", oxygen.Oxygen < 100f);
        oxygen.Exerting = false;

        bool? lastRested = null;
        using var sub = EventBus.Subscribe<RestedChanged>(e => lastRested = e.Rested);

        rested.Rest();
        check.Equal("rest tops up oxygen", 100f, oxygen.Oxygen);
        check.That("the rested bonus is active", rested.Rested);
        check.That("a rested event fired", lastRested == true);

        // XP gains are boosted by 50% while rested.
        CharacterProgress.GainXp(player, 100f);
        check.Equal("rested XP is boosted", 150f, CharacterProgress.Experience(player));

        // Once a full game-day passes, the bonus winds back to the normal rate.
        fake.GameTime = 11.01f;  // just past the one-day window opened at 10.0
        ModHost.Tick(0.1f);
        check.That("the bonus expired", !rested.Rested);
        check.That("an expiry event fired", lastRested == false);

        CharacterProgress.GainXp(player, 10f);
        check.Equal("XP is normal after expiry", 160f, CharacterProgress.Experience(player));

        ModHost.Shutdown();
        CharacterProgress.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
