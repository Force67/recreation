using Recreation.Interop;

namespace Recreation;

// The Civil War war-map channel: managed gameplay pushes each hold's owner and
// the overall war progress, and the engine draws them on the toggle-able war-map
// panel (M). A modern campaign-map readout, the counterpart to Hud's gauges but
// for the strategic board rather than a meter.
public static class WarMap
{
    // Set hold `index`'s display name and owner (0 neutral, 1 Imperial,
    // 2 Stormcloak). Call when the campaign board changes.
    public static void SetHold(int index, string name, int owner) =>
        Global("SetHold", index, name, owner);

    // Set the overall Imperial-controlled fraction (0..1) for the progress bar.
    public static void SetProgress(float imperialFraction) =>
        Global("SetProgress", imperialFraction);

    private static void Global(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("WarMap", function, args);
}
