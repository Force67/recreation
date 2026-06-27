using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield faction reputation: standing accrues and ranks up across the
// thresholds, losses drop the rank back, a rank event fires only on a crossing, and
// the standing is clamped to its band.
public static class StarfieldFactionReputationTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        FactionReputation.Clear();
        EventBus.Clear();

        const uint uc = StarfieldForms.UnitedColoniesFaction;
        const uint freestar = StarfieldForms.FreestarCollectiveFaction;

        FactionRank lastRank = FactionRank.Neutral;
        int rankEvents = 0;
        using var sub = EventBus.Subscribe<FactionRankChanged>(e =>
        {
            if (e.Faction == uc) { lastRank = e.Rank; rankEvents++; }
        });

        check.Equal("starts neutral", FactionRank.Neutral, FactionReputation.RankOf(uc));
        check.Equal("starts at zero standing", 0f, FactionReputation.Of(uc));

        // Earning standing crosses Neutral -> Liked (25) -> Allied (60).
        FactionReputation.Gain(uc, 30f);
        check.Equal("reached Liked", FactionRank.Liked, FactionReputation.RankOf(uc));
        check.Equal("a rank event fired", FactionRank.Liked, lastRank);
        check.Equal("one rank crossing so far", 1, rankEvents);

        // More standing within the same band does not fire again.
        FactionReputation.Gain(uc, 10f);
        check.Equal("still one crossing within a band", 1, rankEvents);
        check.Equal("standing accumulated", 40f, FactionReputation.Of(uc));

        FactionReputation.Gain(uc, 25f);
        check.Equal("reached Allied", FactionRank.Allied, FactionReputation.RankOf(uc));
        check.Equal("two crossings", 2, rankEvents);

        // A betrayal drops the rank back down.
        FactionReputation.Lose(uc, 50f);
        check.Equal("standing fell", 15f, FactionReputation.Of(uc));
        check.Equal("dropped back to Neutral", FactionRank.Neutral, FactionReputation.RankOf(uc));
        check.Equal("three crossings counting the drop", 3, rankEvents);

        // Standing can go hostile below zero.
        FactionReputation.Lose(uc, 40f);
        check.Equal("turned Hostile", FactionRank.Hostile, FactionReputation.RankOf(uc));

        // The band is clamped: a huge gain caps at the ceiling, not beyond.
        FactionReputation.Gain(freestar, 10000f);
        check.Equal("standing clamps at the ceiling", 150f, FactionReputation.Of(freestar));
        check.Equal("a clamped gain still reaches Hero", FactionRank.Hero,
                    FactionReputation.RankOf(freestar));

        // Each faction is tracked independently.
        check.That("factions are independent", FactionReputation.RankOf(uc) != FactionReputation.RankOf(freestar));

        FactionReputation.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
