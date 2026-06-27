using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the game clock derivation and the hourly cue the time service raises.
public static class TimeOfDayTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        // Clock derivation: day 5 at noon.
        fake.GameTime = 5.5f;
        check.Equal("day from game time", 5, GameClock.Day);
        check.Equal("hour from game time", 12, GameClock.Hour);
        check.That("noon is day", GameClock.IsDay);

        int hour = -1;
        int count = 0;
        using var sub = EventBus.Subscribe<GameHourStarted>(e =>
        {
            hour = e.Hour;
            count++;
        });
        ModHost.Register(new TimeOfDayService());

        ModHost.Tick(0.1f);
        check.Equal("first tick announces the hour", 12, hour);
        check.Equal("one cue so far", 1, count);

        // Same hour: no new cue.
        fake.GameTime = 5.51f;
        ModHost.Tick(0.1f);
        check.Equal("same hour raises no cue", 1, count);

        // Next hour: a new cue.
        fake.GameTime = 5.5417f;  // ~13:00
        ModHost.Tick(0.1f);
        check.Equal("new hour announced", 13, hour);
        check.Equal("two cues", 2, count);

        // Into the night.
        fake.GameTime = 5.9f;  // ~21:36
        ModHost.Tick(0.1f);
        check.Equal("night hour announced", 21, hour);
        check.That("21:00 is night", new GameHourStarted(21, 5).IsNight);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
