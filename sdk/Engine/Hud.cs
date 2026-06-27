using Recreation.Interop;

namespace Recreation;

// Persistent on-screen gauges, the counterpart to Debug.Notification's transient
// toast. A gameplay system (oxygen, radiation, adrenaline, ...) pushes a named
// bar each tick and the engine draws it on the HUD; clearing one removes it. The
// everyday way a survival meter reaches the player, mirroring the Papyrus-era
// convention of a custom HUD widget but driven entirely from managed code.
public static class Hud
{
    // Show or update a labeled gauge. `fraction` is clamped to 0..1 by the engine;
    // `color` is packed rgba8 (0 lets the HUD pick a colour). Call each tick from
    // the owning system so the bar tracks the live value.
    public static void Gauge(string id, float fraction, string label, uint color = 0) =>
        Global("Gauge", id, fraction, label, (int)color);

    // Remove a gauge's bar (e.g. when its meter is full and no longer relevant).
    public static void ClearGauge(string id) => Global("ClearGauge", id);

    private static void Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Hud", function, args);
}
