using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield's jurisdictional bounty: a crime raises the bounty in that
// jurisdiction and docks standing with it, paying credits draws it down (capped at
// what is owed and at what the player can afford), a pardon wipes it, and each
// jurisdiction is tracked on its own.
public static class StarfieldBountiesTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        Bounties.Clear();
        FactionReputation.Clear();
        Economy.Clear();
        EventBus.Clear();

        const uint uc = StarfieldForms.UnitedColoniesFaction;
        const uint freestar = StarfieldForms.FreestarCollectiveFaction;
        Actor player = Game.Player;

        int lastBounty = -1;
        using var sub = EventBus.Subscribe<BountyChanged>(e =>
        {
            if (e.Jurisdiction == uc) lastBounty = e.Bounty;
        });

        check.That("no bounty to start", !Bounties.IsWanted(uc));

        // A crime raises the UC bounty and costs UC standing.
        Bounties.Report(uc, 500);
        check.Equal("bounty added", 500, Bounties.BountyIn(uc));
        check.That("now wanted", Bounties.IsWanted(uc));
        check.Equal("a bounty event fired", 500, lastBounty);
        check.That("reputation took a hit", FactionReputation.Of(uc) < 0f);

        // A second crime accumulates.
        Bounties.Report(uc, 250);
        check.Equal("bounty accumulates", 750, Bounties.BountyIn(uc));

        // A non-positive report is a no-op.
        Bounties.Report(uc, 0);
        check.Equal("a zero report is harmless", 750, Bounties.BountyIn(uc));

        // Paying needs credits: with none, nothing is paid.
        check.Equal("cannot pay with no credits", 0, Bounties.Pay(player, uc, 1000));
        check.Equal("bounty unchanged after a failed pay", 750, Bounties.BountyIn(uc));

        // A partial payment draws the bounty down by what was spent.
        Economy.Add(player, 2000);
        check.Equal("partial payment", 300, Bounties.Pay(player, uc, 300));
        check.Equal("bounty drawn down", 450, Bounties.BountyIn(uc));
        check.Equal("credits spent on the fine", 1700L, Economy.Credits(player));

        // Overpaying is capped at what is owed; only the owed amount is spent.
        check.Equal("overpay is capped at what is owed", 450, Bounties.Pay(player, uc, 99999));
        check.Equal("bounty cleared by paying", 0, Bounties.BountyIn(uc));
        check.Equal("only the owed amount was spent", 1250L, Economy.Credits(player));
        check.That("no longer wanted", !Bounties.IsWanted(uc));

        // A pardon wipes a bounty without payment.
        Bounties.Report(freestar, 800);
        Bounties.Forgive(freestar);
        check.Equal("a pardon clears the bounty", 0, Bounties.BountyIn(freestar));

        // Jurisdictions are independent: a Freestar crime does not touch UC.
        Bounties.Report(freestar, 100);
        check.Equal("UC stays clear", 0, Bounties.BountyIn(uc));
        check.Equal("Freestar carries its own bounty", 100, Bounties.BountyIn(freestar));

        Bounties.Clear();
        FactionReputation.Clear();
        Economy.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
