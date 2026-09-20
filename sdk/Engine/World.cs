using Recreation.Interop;

namespace Recreation;

// The shared world: what time it is and what the sky is doing. Both are facts
// about the session rather than about a player, so a host that changes either
// changes it for everyone connected -- the engine replicates the clock and the
// weather seed, and every client adopts them (see the main README's "World
// sync"). Server-authoritative by nature: a client can call these, but the
// host's next world-state message puts the shared answer back.
//
// Reading the clock is GameClock's job; this is the writing side.
public static class World
{
    // Sets the time of day, in hours in [0, 24) -- 13.5 is half past one in the
    // afternoon. The day count is kept, so this moves the hour, not the date.
    public static void SetTime(float hour) => Global("SetTime", Value.Float(hour));

    // Sets the time of day from whole hours and minutes.
    public static void SetTime(int hour, int minute) => SetTime(hour + minute / 60f);

    // Brings in a weather (a WTHR form) now, cross-fading from whatever is
    // overhead. It then goes on evolving normally from there rather than pinning
    // -- the weather that follows is the one the climate would have given after
    // this one. No-op when the form is not a weather this game authored.
    public static void SetWeather(Form weather) => SetWeather(weather.Handle);

    public static void SetWeather(ulong weatherHandle) =>
        Global("SetWeather", Value.Object(weatherHandle));

    private static Value Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("World", function, args);
}
