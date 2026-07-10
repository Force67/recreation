using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Confirms the thin global-API wrappers dispatch to the right native with the
// right arguments. Catches name and argument-order typos in the wrappers.
public static class SdkApiTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        Debug.Notification("hello");
        check.Equal("Debug.Notification type", "Debug", fake.LastGlobalType);
        check.Equal("Debug.Notification function", "Notification", fake.LastGlobalFunction);
        check.Equal("Debug.Notification passes the message", "hello", fake.LastStringArg);

        Debug.Trace("log line");
        check.Equal("Debug.Trace function", "Trace", fake.LastGlobalFunction);

        int r = Utility.RandomInt(7, 9);
        check.Equal("Utility.RandomInt type", "Utility", fake.LastGlobalType);
        check.Equal("Utility.RandomInt returns engine value", 7, r);

        // Rand routes through the engine PRNG (the fake returns the minimum).
        check.Equal("Rand.Range int", 3, Rand.Range(3, 8));
        check.Equal("Rand.Range float", 1.5f, Rand.Range(1.5f, 4f));
        check.That("Rand.Chance uses Value", Rand.Chance(0.5f));  // Value == 0 < 0.5

        // Spatial helpers compose the position natives and Vector3 math.
        const ulong from = 0x10;
        const ulong to = 0x20;
        fake.SetPosition(from, 0, 0, 0);
        fake.SetPosition(to, 0, 10, 0);
        Vector3 dir = ObjectReference.From(from).DirectionTo(ObjectReference.From(to));
        check.Equal("direction is a unit vector toward target", new Vector3(0, 1, 0), dir);

        ObjectReference.From(from).Translate(new Vector3(5, 0, 0));
        check.That("translate shifts the reference", fake.GetPosition(from).X == 5f);

        // Form.Is matches the engine's script type for a handle (case-insensitive).
        const ulong scripted = 0x30;
        fake.SetType(scripted, "Ticker");
        check.That("Form.Is matches the type", Form.From(scripted).Is("ticker"));
        check.That("Form.Is rejects a different type", !Form.From(scripted).Is("Actor"));

        // Game typed getters resolve a form id through GetForm.
        check.Equal("Game.GetActor resolves the id", 0x99UL, Game.GetActor(0x99).Handle);
        check.Equal("Game.GetQuest resolves the id", 0xABUL, Game.GetQuest(0xAB).Handle);
        check.Equal("Game.GetFaction resolves the id", 0xCDUL, Game.GetFaction(0xCD).Handle);

        Native.Backend = null;
    }
}
