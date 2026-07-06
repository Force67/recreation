using System.Collections.Generic;

namespace Recreation;

// Coordinates the fast-travel gate across systems. Several systems (combat,
// encumbrance, being indoors, ...) each have a reason to forbid fast travel; if
// each toggled the engine flag directly they would fight, one re-enabling it
// while another still wants it closed. Instead they register a named reason
// here: fast travel is blocked while any reason holds, and reopens only when the
// last one clears. The engine flag is touched only on the aggregate transition.
public static class FastTravel
{
    private static readonly HashSet<string> Blocks = new();

    public static bool IsBlocked => Blocks.Count > 0;

    // Forbids fast travel for `reason`. Idempotent per reason.
    public static void Block(string reason)
    {
        bool wasOpen = Blocks.Count == 0;
        if (Blocks.Add(reason) && wasOpen) Game.EnableFastTravel(false);
    }

    // Clears `reason`; fast travel reopens only if no other reason remains.
    public static void Unblock(string reason)
    {
        if (Blocks.Remove(reason) && Blocks.Count == 0) Game.EnableFastTravel(true);
    }

    // Drops every reason and reopens fast travel. Used on managed-world teardown.
    public static void Clear()
    {
        if (Blocks.Count == 0) return;
        Blocks.Clear();
        Game.EnableFastTravel(true);
    }
}
