using System;
using Recreation.Interop;

namespace Recreation;

// Exercises the SDK against a live engine guest: native dispatch (Game.Player
// and the actor-value/state calls it implies) and the instance lifecycle
// (create, call, tick, read a property). The native hosttest harness wires a
// fixture with known values (player handle 0x14, Health 100, Level 5) and a
// Ticker script, then asserts Run() returns 0.
internal static class SdkSelfTest
{
    public static int Run()
    {
        int failures = 0;
        void Check(string what, bool ok)
        {
            Console.WriteLine($"  [selftest] {what,-40} {(ok ? "ok" : "FAIL")}");
            if (!ok) failures++;
        }

        // Native dispatch through the ergonomic wrappers.
        Actor player = Game.Player;
        Check("player handle is 0x14", player.Handle == 0x14);
        Check("player health == 100", player.Health == 100f);
        Check("player level == 5", player.Level == 5);
        Check("player not dead", !player.IsDead);
        Check("player in combat", player.IsInCombat);
        Check("player position x == 1", player.Position.X == 1f);

        // Instance lifecycle + auto property + scheduled update.
        if (Native.IsScriptLoaded("Ticker"))
        {
            ulong ticker = Native.CreateInstance("Ticker");
            Check("ticker instantiated", ticker != 0);
            Native.CallMethod(ticker, "begin", default);  // schedules one update 0.5s out
            Native.Tick(0.6f);                             // crosses the timer
            Check("ticker Count == 1 after tick", Native.GetProperty(ticker, "Count").AsInt() == 1);
        }
        else
        {
            Check("ticker script loaded", false);
        }

        Console.WriteLine($"[selftest] {failures} failure(s)");
        return failures;
    }
}
