using Recreation.Games.Skyrim;

namespace Recreation.Tests;

// Covers the Civil War campaign board: the canonical war-start ownership, that a
// capture flips a hold (and only fires once for a real change), the hold counts,
// and the derived war progress. Pure model, so no backend is needed.
public static class CivilWarCampaignTests
{
    public static void Run(Check check)
    {
        CivilWarCampaign.Reset();

        // Canonical war-start board: Empire west/north-west, Stormcloaks east,
        // Whiterun neutral.
        check.Equal("Haafingar starts Imperial", Side.Imperial, CivilWarCampaign.OwnerOf(Hold.Haafingar));
        check.Equal("The Reach starts Imperial", Side.Imperial, CivilWarCampaign.OwnerOf(Hold.TheReach));
        check.Equal("Falkreath starts Imperial", Side.Imperial, CivilWarCampaign.OwnerOf(Hold.Falkreath));
        check.Equal("Hjaalmarch starts Imperial", Side.Imperial, CivilWarCampaign.OwnerOf(Hold.Hjaalmarch));
        check.Equal("Eastmarch starts Stormcloak", Side.Stormcloak, CivilWarCampaign.OwnerOf(Hold.Eastmarch));
        check.Equal("The Rift starts Stormcloak", Side.Stormcloak, CivilWarCampaign.OwnerOf(Hold.TheRift));
        check.Equal("Winterhold starts Stormcloak", Side.Stormcloak, CivilWarCampaign.OwnerOf(Hold.Winterhold));
        check.Equal("The Pale starts Stormcloak", Side.Stormcloak, CivilWarCampaign.OwnerOf(Hold.ThePale));
        check.Equal("Whiterun starts neutral", Side.Neutral, CivilWarCampaign.OwnerOf(Hold.Whiterun));

        check.Equal("four Imperial holds at start", 4, CivilWarCampaign.ImperialHoldCount);
        check.Equal("four Stormcloak holds at start", 4, CivilWarCampaign.StormcloakHoldCount);
        check.Equal("imperial war progress is 4/9", 4f / 9f, CivilWarCampaign.WarProgress(Side.Imperial));

        // A capture flips ownership and raises the event exactly once.
        int events = 0;
        Hold capturedHold = Hold.Eastmarch;
        Side capturedSide = Side.Neutral;
        void OnCaptured(Hold h, Side s) { events++; capturedHold = h; capturedSide = s; }
        CivilWarCampaign.HoldCaptured += OnCaptured;

        check.That("taking neutral Whiterun changes ownership",
                   CivilWarCampaign.CaptureHold(Hold.Whiterun, Side.Imperial));
        check.Equal("Whiterun is now Imperial", Side.Imperial, CivilWarCampaign.OwnerOf(Hold.Whiterun));
        check.Equal("Imperial holds rise to five", 5, CivilWarCampaign.ImperialHoldCount);
        check.Equal("one capture event", 1, events);
        check.Equal("event carried the hold", Hold.Whiterun, capturedHold);
        check.Equal("event carried the captor", Side.Imperial, capturedSide);

        // Recapturing a hold the side already controls is a no-op.
        check.That("recapturing for the same side is a no-op",
                   !CivilWarCampaign.CaptureHold(Hold.Whiterun, Side.Imperial));
        check.Equal("no extra event for a no-op", 1, events);

        // Taking an enemy hold moves both counts.
        check.That("Imperials take The Rift from the Stormcloaks",
                   CivilWarCampaign.CaptureHold(Hold.TheRift, Side.Imperial));
        check.Equal("Imperial holds at six", 6, CivilWarCampaign.ImperialHoldCount);
        check.Equal("Stormcloak holds fall to three", 3, CivilWarCampaign.StormcloakHoldCount);
        check.Equal("imperial war progress is 6/9", 6f / 9f, CivilWarCampaign.WarProgress(Side.Imperial));

        CivilWarCampaign.HoldCaptured -= OnCaptured;

        // AdvanceFront takes the contested seat first, then enemy holds, and a
        // neutral attacker takes nothing.
        CivilWarCampaign.Reset();
        check.Equal("neutral attacker advances nothing", (Hold?)null,
                    CivilWarCampaign.AdvanceFront(Side.Neutral));
        check.Equal("Imperial advance takes contested Whiterun first", (Hold?)Hold.Whiterun,
                    CivilWarCampaign.AdvanceFront(Side.Imperial));
        check.Equal("Whiterun is now Imperial", Side.Imperial, CivilWarCampaign.OwnerOf(Hold.Whiterun));
        check.Equal("five Imperial holds after the advance", 5, CivilWarCampaign.ImperialHoldCount);
        Hold? next = CivilWarCampaign.AdvanceFront(Side.Imperial);
        bool tookEnemyHold = next is Hold h && CivilWarCampaign.OwnerOf(h) == Side.Imperial;
        check.Equal("next Imperial advance takes a Stormcloak hold", true, tookEnemyHold);
        check.Equal("Stormcloak holds fall to three after two advances", 3,
                    CivilWarCampaign.StormcloakHoldCount);

        // Teardown restores the canonical board.
        CivilWarCampaign.Reset();
        check.Equal("reset restores four Imperial holds", 4, CivilWarCampaign.ImperialHoldCount);
        check.Equal("reset restores Whiterun neutral", Side.Neutral, CivilWarCampaign.OwnerOf(Hold.Whiterun));
    }
}
