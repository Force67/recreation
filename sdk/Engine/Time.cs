namespace Recreation;

// Frame timing in the Unity idiom: how long the current frame represents, how long
// the world has run, and which frame it is, read anywhere without subscribing to
// the frame event. The host advances it once at the top of every frame, so within
// a frame every system sees the same values. GameTime bridges to the in-world
// clock for mods that key off the day or hour rather than real time.
public static class Time
{
    // Seconds the current frame advances (the value handed to OnUpdate).
    public static float DeltaTime { get; private set; }

    // Real seconds elapsed since the managed world started.
    public static float Elapsed { get; private set; }

    // Frames elapsed since the managed world started.
    public static long FrameCount { get; private set; }

    // In-world time in days (the engine's game clock), for mods that key off the
    // day or hour rather than real time.
    public static float GameTime => GameClock.GameTime;

    // Advances the clock by one frame. The host calls this at the top of its tick;
    // mods never call it.
    internal static void Advance(float deltaTime)
    {
        DeltaTime = deltaTime;
        Elapsed += deltaTime;
        FrameCount++;
    }

    // Resets to a fresh world on teardown, so a reload starts at zero.
    internal static void Reset()
    {
        DeltaTime = 0f;
        Elapsed = 0f;
        FrameCount = 0;
    }
}
