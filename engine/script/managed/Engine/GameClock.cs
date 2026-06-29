using System;
using Recreation.Interop;

namespace Recreation;

// The in-game clock, derived from the engine's game time. A convenience over the
// raw Utility.GetCurrentGameTime so mods read the day, the hour and whether it is
// night without doing the arithmetic. Game time is measured in days since the
// game began; the fractional part is the time of day.
public static class GameClock
{
    // Days since the game began (the integer part is the day, the fraction the
    // time of day).
    public static float GameTime => Utility.CurrentGameTime;

    public static int Day => (int)MathF.Floor(GameTime);

    // Hour of the day as a float in [0, 24).
    public static float HourOfDay => (GameTime - MathF.Floor(GameTime)) * 24f;

    // Hour of the day as a whole number in [0, 23].
    public static int Hour => (int)HourOfDay;

    // Skyrim's day runs roughly 06:00-20:00; outside that is night.
    public static bool IsNightHour(int hour) => hour < 6 || hour >= 20;

    public static bool IsNight => IsNightHour(Hour);
    public static bool IsDay => !IsNight;
}
