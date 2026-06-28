using Recreation.Games.Skyrim;

namespace Recreation.Tests;

// Covers the Civil War rank ladder: side-correct titles at each index, that
// completing missions promotes the player one rank at a time up to the honorary
// post-war title, that the rank event fires on real promotions only, and that reset
// clears progression. Pure model, so no backend is needed.
public static class CivilWarRankTests
{
    public static void Run(Check check)
    {
        CivilWarRank.Reset();
        CivilWarState.Reset();

        // Side-correct titles along each ladder.
        check.Equal("Imperial rank 0 is Quaestor", "Quaestor", CivilWarRank.RankTitle(CivilWarSide.Imperial, 0));
        check.Equal("Imperial rank 3 is Legate", "Legate", CivilWarRank.RankTitle(CivilWarSide.Imperial, 3));
        check.Equal("Stormcloak rank 0 is Unblooded", "Unblooded", CivilWarRank.RankTitle(CivilWarSide.Stormcloak, 0));
        check.Equal("Stormcloak top rank is Stormblade", "Stormblade",
                    CivilWarRank.RankTitle(CivilWarSide.Stormcloak, CivilWarRank.MaxRankIndex));
        check.Equal("an out-of-range index clamps to the top rank", "Hero of the Legion",
                    CivilWarRank.RankTitle(CivilWarSide.Imperial, 99));
        check.Equal("an unaligned side has no title", "", CivilWarRank.RankTitle(CivilWarSide.None, 0));

        // The player joins the Legion and starts unranked at the bottom.
        CivilWarState.Set(CivilWarSide.Imperial);
        check.Equal("a fresh recruit is rank 0", 0, CivilWarRank.CurrentRankIndex);
        check.Equal("current rank reads the joined side", "Quaestor", CivilWarRank.CurrentRank);

        int promotions = 0;
        CivilWarSide lastSide = CivilWarSide.None;
        int lastIndex = -1;
        void OnRank(CivilWarSide s, int i) { promotions++; lastSide = s; lastIndex = i; }
        CivilWarRank.RankChanged += OnRank;

        // Each completed mission promotes the player one rank, and the title advances.
        check.That("first mission promotes", CivilWarRank.Promote());
        check.Equal("now a Praefect", "Praefect", CivilWarRank.CurrentRank);
        check.Equal("one promotion event", 1, promotions);
        check.Equal("event carried the side", CivilWarSide.Imperial, lastSide);
        check.Equal("event carried the new index", 1, lastIndex);

        check.That("second mission promotes", CivilWarRank.Promote());
        check.Equal("now a Tribune", "Tribune", CivilWarRank.CurrentRank);
        check.That("third mission promotes", CivilWarRank.Promote());
        check.Equal("now a Legate", "Legate", CivilWarRank.CurrentRank);
        check.That("final mission earns the post-war title", CivilWarRank.Promote());
        check.Equal("now Hero of the Legion", "Hero of the Legion", CivilWarRank.CurrentRank);
        check.Equal("reached the top rank index", CivilWarRank.MaxRankIndex, CivilWarRank.CurrentRankIndex);
        check.Equal("four promotions to climb the ladder", 4, promotions);

        // The ladder caps: a mission past the top rank neither promotes nor re-toasts.
        check.That("a mission at the top rank does not promote", !CivilWarRank.Promote());
        check.Equal("no extra promotion event at the cap", 4, promotions);
        check.Equal("still the top rank", "Hero of the Legion", CivilWarRank.CurrentRank);

        CivilWarRank.RankChanged -= OnRank;

        // SetRank jumps directly and the title follows the player's (new) side.
        CivilWarRank.Reset();
        CivilWarState.Reset();
        CivilWarState.Set(CivilWarSide.Stormcloak);
        check.That("setting rank 2 changes the rank", CivilWarRank.SetRank(2));
        check.Equal("Stormcloak rank 2 is Ice-Veins", "Ice-Veins", CivilWarRank.CurrentRank);
        check.That("setting the same rank is a no-op", !CivilWarRank.SetRank(2));

        // Reset clears progression back to unranked.
        CivilWarRank.Reset();
        check.Equal("reset zeroes completed missions", 0, CivilWarRank.CompletedMissions);
        check.Equal("reset returns to rank 0", 0, CivilWarRank.CurrentRankIndex);

        CivilWarState.Reset();
    }
}
