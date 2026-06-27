using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers faction reputation: standing moves up and down within bounds, the numeric
// value bands into Hostile/Neutral/Friendly/Allied, band crossings raise events,
// and taking a side pays one faction while docking its rival.
public static class FalloutFactionReputationTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        FactionReputation.Clear();
        EventBus.Clear();

        int crossings = 0;
        FactionStanding last = FactionStanding.Neutral;
        using var sub = EventBus.Subscribe<FactionStandingChanged>(e => { last = e.Standing; crossings++; });

        // Everyone starts Neutral at zero.
        check.Equal("starts at zero", 0, FactionReputation.Of(CommonwealthFaction.Minutemen));
        check.Equal("starts Neutral", FactionStanding.Neutral,
                    FactionReputation.StandingOf(CommonwealthFaction.Minutemen));

        // Helping the Minutemen climbs into Friendly then Allied.
        FactionReputation.Gain(CommonwealthFaction.Minutemen, 30);
        check.Equal("crossed into Friendly", FactionStanding.Friendly,
                    FactionReputation.StandingOf(CommonwealthFaction.Minutemen));
        FactionReputation.Gain(CommonwealthFaction.Minutemen, 50);  // 80 >= 75
        check.Equal("crossed into Allied", FactionStanding.Allied, last);
        check.That("Minutemen allied", FactionReputation.IsAllied(CommonwealthFaction.Minutemen));
        check.Equal("two band crossings so far", 2, crossings);

        // A small gain that stays in the same band raises no event.
        int before = crossings;
        FactionReputation.Gain(CommonwealthFaction.Minutemen, 5);
        check.Equal("no crossing within a band", before, crossings);

        // Reputation clamps at the ceiling.
        FactionReputation.Gain(CommonwealthFaction.Minutemen, 1000);
        check.Equal("clamps at max", FactionReputation.Max, FactionReputation.Of(CommonwealthFaction.Minutemen));

        // Taking a side: pay the Institute, dock the Railroad.
        FactionReputation.GainWithRival(CommonwealthFaction.Institute, CommonwealthFaction.Railroad, 60);
        check.Equal("the Institute gained", 60, FactionReputation.Of(CommonwealthFaction.Institute));
        check.Equal("the Railroad was docked", -60, FactionReputation.Of(CommonwealthFaction.Railroad));
        check.That("the Railroad turned Hostile", FactionReputation.IsHostile(CommonwealthFaction.Railroad));

        // Reputation clamps at the floor too.
        FactionReputation.Gain(CommonwealthFaction.Railroad, -1000);
        check.Equal("clamps at min", FactionReputation.Min, FactionReputation.Of(CommonwealthFaction.Railroad));

        FactionReputation.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
