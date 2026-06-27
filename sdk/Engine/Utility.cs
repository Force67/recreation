using Recreation.Interop;

namespace Recreation;

// Engine-wide helpers from the Papyrus Utility script: randomness tied to the
// engine's deterministic PRNG (so scripted randomness replicates), and the game
// and real clocks. The blocking Wait* calls are intentionally omitted; managed
// code schedules over time with behaviours and timers instead.
public static class Utility
{
    // A random integer in [min, max].
    public static int RandomInt(int min, int max) => Global("RandomInt", min, max).AsInt();

    // A random float in [min, max].
    public static float RandomFloat(float min, float max) =>
        Global("RandomFloat", min, max).AsFloat();

    // In-game time, in days since the game began.
    public static float CurrentGameTime => Global("GetCurrentGameTime").AsFloat();

    // Wall-clock seconds since the session started.
    public static float CurrentRealTime => Global("GetCurrentRealTime").AsFloat();

    public static bool IsInMenuMode => Global("IsInMenuMode").AsBool();

    private static Value Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Utility", function, args);
}
