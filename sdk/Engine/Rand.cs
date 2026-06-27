using Recreation.Interop;

namespace Recreation;

// Random numbers in the Unity idiom, backed by the engine's deterministic PRNG
// (Utility.Random*). Routing through the engine means scripted randomness is
// reproducible and replicates across a multiplayer session, instead of each
// client rolling its own.
public static class Rand
{
    // A random int in [minInclusive, maxInclusive].
    public static int Range(int minInclusive, int maxInclusive) =>
        Utility.RandomInt(minInclusive, maxInclusive);

    // A random float in [min, max).
    public static float Range(float min, float max) => Utility.RandomFloat(min, max);

    // A random float in [0, 1).
    public static float Value => Utility.RandomFloat(0f, 1f);

    // True with the given probability (0..1).
    public static bool Chance(float probability) => Value < probability;
}
