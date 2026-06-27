using Recreation.Modding;

namespace Recreation;

// Coordinates disabling the player's controls across systems, the way a scripted
// sequence (a hotkey-triggered meditation, a custom cutscene) takes control and
// returns it. Like the fast-travel gate, each system holds a named reason; the
// controls are disabled while any reason holds and re-enabled only when the last
// clears, so overlapping sequences do not hand control back early.
public static class PlayerControls
{
    private static readonly Gate Gate = new(open =>
    {
        if (open) Game.EnablePlayerControls();
        else Game.DisablePlayerControls();
    });

    public static bool Disabled => Gate.IsClosed;

    public static void Disable(string reason) => Gate.Close(reason);
    public static void Enable(string reason) => Gate.Open(reason);
    public static void Clear() => Gate.Reset();
}
