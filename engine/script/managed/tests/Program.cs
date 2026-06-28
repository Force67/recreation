using System;

namespace Recreation.Tests;

// The managed test runner: executes each suite against a shared Check, prints a
// summary and exits with the failure count so CI (and `dotnet run`) gate on it.
internal static class Program
{
    private static int Main()
    {
        var check = new Check();

        ValueTests.Run(check);
        SdkApiTests.Run(check);
        EventBusTests.Run(check);
        EngineEventsTests.Run(check);
        ModHostTests.Run(check);
        ModLoaderTests.Run(check);
        ModConfigTests.Run(check);
        SchedulerTests.Run(check);
        FormScriptsTests.Run(check);
        SkyrimRegenTests.Run(check);
        SkyrimEventTests.Run(check);

        Console.WriteLine($"[tests] {check.Passed} passed, {check.Failed} failed");
        return check.Failed;
    }
}
