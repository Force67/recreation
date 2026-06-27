using Recreation.Modding;

namespace Recreation;

// Coordinates the fast-travel gate across systems. Several systems (combat,
// encumbrance, being indoors, ...) each have a reason to forbid fast travel; if
// each toggled the engine flag directly they would fight, one re-enabling it
// while another still wants it closed. Each registers a named reason instead:
// fast travel is blocked while any reason holds and reopens only when the last
// clears, with the engine flag touched only on the aggregate transition.
public static class FastTravel
{
    private static readonly Gate Gate = new(open => Game.EnableFastTravel(open));

    public static bool IsBlocked => Gate.IsClosed;

    public static void Block(string reason) => Gate.Close(reason);
    public static void Unblock(string reason) => Gate.Open(reason);
    public static void Clear() => Gate.Reset();
}
