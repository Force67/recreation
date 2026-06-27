using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the reason-based gate primitive and the player-controls gate built on
// it (the fast-travel gate is covered by FastTravelTests).
public static class GateTests
{
    public static void Run(Check check)
    {
        // The primitive: closed while any reason holds, callback only on the edge.
        int changes = 0;
        bool open = true;
        var gate = new Gate(o =>
        {
            open = o;
            changes++;
        });

        gate.Close("a");
        gate.Close("b");
        check.Equal("closes once for two reasons", 1, changes);
        check.That("closed while a reason holds", gate.IsClosed && !open);

        gate.Open("a");
        check.That("stays closed with a reason left", gate.IsClosed);
        gate.Open("b");
        check.Equal("opens once when the last clears", 2, changes);
        check.That("open with no reasons", !gate.IsClosed && open);

        // The player-controls gate over the engine flag.
        var fake = new FakeBackend();
        Native.Backend = fake;
        PlayerControls.Clear();

        check.That("controls enabled by default", fake.PlayerControlsEnabled);
        PlayerControls.Disable("cutscene");
        PlayerControls.Disable("menu");
        check.That("controls disabled while held", !fake.PlayerControlsEnabled && PlayerControls.Disabled);
        PlayerControls.Enable("cutscene");
        check.That("stays disabled with a reason left", !fake.PlayerControlsEnabled);
        PlayerControls.Enable("menu");
        check.That("controls restored when the last clears", fake.PlayerControlsEnabled);

        PlayerControls.Clear();
        Native.Backend = null;
    }
}
