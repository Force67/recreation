using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the shared-world writing surface: a mod moves the hour and the sky, and
// the call reaches the engine in the shape the runtime routes (World.SetTime
// takes hours in [0,24), World.SetWeather takes a form handle). The engine side
// replicates both to every client; here we only assert what leaves managed code.
public static class WorldTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        World.SetTime(13.5f);
        check.Equal("the hour reaches the engine", 13.5f, fake.LastWorldHour ?? -1f);

        World.SetTime(7, 30);
        check.Equal("hours and minutes fold into one hour", 7.5f, fake.LastWorldHour ?? -1f);

        World.SetWeather(0x0000000100010DC2UL);
        check.Equal("the weather form reaches the engine", 0x0000000100010DC2UL,
                    fake.LastWorldWeather ?? 0UL);

        // A Form overload so a mod that resolved a WTHR can pass it straight in.
        World.SetWeather(Form.From(0x42UL));
        check.Equal("a form passes its handle", 0x42UL, fake.LastWorldWeather ?? 0UL);
    }
}
