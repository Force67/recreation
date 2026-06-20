using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Unity-style Time clock: the host advances it once per frame, so delta
// tracks the latest frame while elapsed and the frame count accumulate, and a
// teardown resets it.
public static class TimeTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();  // a fresh world starts the clock at zero

        check.Equal("delta starts at zero", 0f, Time.DeltaTime);
        check.Equal("elapsed starts at zero", 0f, Time.Elapsed);
        check.Equal("frame count starts at zero", 0L, Time.FrameCount);

        ModHost.Tick(0.5f);
        check.Equal("delta tracks the frame", 0.5f, Time.DeltaTime);
        check.Equal("elapsed accumulates", 0.5f, Time.Elapsed);
        check.Equal("one frame counted", 1L, Time.FrameCount);

        ModHost.Tick(0.25f);
        check.Equal("delta is the latest frame", 0.25f, Time.DeltaTime);
        check.Equal("elapsed sums the frames", 0.75f, Time.Elapsed);
        check.Equal("frames keep counting", 2L, Time.FrameCount);

        // GameTime bridges to the in-world clock.
        fake.GameTime = 3.5f;
        check.Equal("GameTime reads the world clock", 3.5f, Time.GameTime);

        ModHost.Shutdown();
        check.Equal("teardown resets elapsed", 0f, Time.Elapsed);
        check.Equal("teardown resets the frame count", 0L, Time.FrameCount);

        Native.Backend = null;
    }
}
