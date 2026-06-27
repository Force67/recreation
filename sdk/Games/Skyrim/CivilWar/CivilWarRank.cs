using System;

namespace Recreation.Games.Skyrim;

// The player's standing in the Civil War: a field rank that climbs as their side's
// campaign advances. Each side has its own ladder of titles, low to high, ending in
// an honorary post-war title the victors hail the player with. A modern-RPG touch,
// vanilla scatters these ranks across dialogue, here they are one tracked
// progression with a promotion toast.
//
// Pure C# and self-contained: the rank index is derived from a count of completed
// Civil War missions (hold captures), so it is deterministic and testable without
// the engine. The tracker behaviour feeds Promote() from campaign events; nothing
// here runs on a timer or calls native. Reset() clears it on teardown.
public static class CivilWarRank
{
    // Imperial Legion ranks, low to high. The first four are the canonical field
    // ranks; the last is the honorary title the Legion gives once the war is won.
    private static readonly string[] ImperialTitles =
    {
        "Quaestor", "Praefect", "Tribune", "Legate", "Hero of the Legion",
    };

    // Stormcloak ranks, low to high, ending in Stormblade, the title Ulfric grants
    // the player when Skyrim is liberated.
    private static readonly string[] StormcloakTitles =
    {
        "Unblooded", "Bone-Breaker", "Ice-Veins", "Snow-Hammer", "Stormblade",
    };

    // Top rank index. Both ladders are the same length, so one value covers both.
    public static int MaxRankIndex { get; } = ImperialTitles.Length - 1;

    // Completed Civil War missions (hold captures) credited to the player. The rank
    // index is derived from this, so a promotion is just "one more mission done".
    public static int CompletedMissions { get; private set; }

    // The player's rank index, derived from missions completed and clamped to the
    // ladder. One promotion per mission until the top rank.
    public static int CurrentRankIndex => RankIndexForMissions(CompletedMissions);

    // The player's current rank title, for the side they have joined. Empty while
    // unaligned (CivilWarState.CurrentSide is None).
    public static string CurrentRank => RankTitle(CivilWarState.CurrentSide, CurrentRankIndex);

    // Raised when the rank index actually advances, carrying the player's side and
    // the new rank index. The tracker listens to toast the promotion.
    public static event Action<CivilWarSide, int>? RankChanged;

    // The rank a given number of completed missions earns: one rank per mission,
    // clamped to the ladder. The single knob mapping campaign progress to rank.
    public static int RankIndexForMissions(int missions) =>
        Math.Clamp(missions, 0, MaxRankIndex);

    // The title at `index` on a side's ladder, clamped into range. A neutral/None
    // side has no ladder, so it returns the empty string.
    public static string RankTitle(CivilWarSide side, int index) => side switch
    {
        CivilWarSide.Imperial => ImperialTitles[Clamp(index)],
        CivilWarSide.Stormcloak => StormcloakTitles[Clamp(index)],
        _ => string.Empty,
    };

    // Credits one completed mission and promotes the player if it lifts their rank.
    // Returns true only on a real promotion, so a re-fired campaign event does not
    // double-toast and reaching the top rank stops raising the event.
    public static bool Promote()
    {
        int before = CurrentRankIndex;
        CompletedMissions++;
        int after = CurrentRankIndex;
        if (after == before) return false;
        RankChanged?.Invoke(CivilWarState.CurrentSide, after);
        return true;
    }

    // Forces the rank to a specific index (e.g. restoring a save), syncing the
    // mission count to match. Raises RankChanged only when the index changes.
    public static bool SetRank(int index)
    {
        index = Clamp(index);
        int before = CurrentRankIndex;
        CompletedMissions = index;  // 1:1 mapping: index missions earns rank index
        if (index == before) return false;
        RankChanged?.Invoke(CivilWarState.CurrentSide, index);
        return true;
    }

    // Clears progression on teardown so a reload starts the player unranked.
    public static void Reset() => CompletedMissions = 0;

    private static int Clamp(int index) => Math.Clamp(index, 0, MaxRankIndex);
}
