using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield crew affinity: approval and disapproval move the meter across the
// like/admire and dislike/hate tiers, a threshold event fires only on a crossing,
// reaching Admire unlocks the companion perk once and it stays unlocked through a
// later dip, and each companion is tracked on its own.
public static class StarfieldCrewAffinityTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        CrewAffinity.Clear();
        EventBus.Clear();

        Actor sarah = Actor.From(0x500);
        Actor sam = Actor.From(0x501);

        AffinityTier lastTier = AffinityTier.Neutral;
        int tierEvents = 0, perkEvents = 0;
        using var s1 = EventBus.Subscribe<AffinityThresholdChanged>(e =>
        {
            if (e.CrewHandle == sarah.Handle) { lastTier = e.Tier; tierEvents++; }
        });
        using var s2 = EventBus.Subscribe<CompanionPerkUnlocked>(e =>
        {
            if (e.CrewHandle == sarah.Handle) perkEvents++;
        });

        check.Equal("starts neutral", AffinityTier.Neutral, CrewAffinity.TierOf(sarah));
        check.That("no perk to start", !CrewAffinity.HasPerk(sarah));

        // Approval crosses Neutral -> Like (30).
        CrewAffinity.Approve(sarah, 30f);
        check.Equal("reached Like", AffinityTier.Like, CrewAffinity.TierOf(sarah));
        check.Equal("a threshold event fired", AffinityTier.Like, lastTier);
        check.Equal("one crossing so far", 1, tierEvents);

        // More approval inside the band does not re-fire.
        CrewAffinity.Approve(sarah, 10f);
        check.Equal("still one crossing within Like", 1, tierEvents);

        // Reaching Admire (80) unlocks the perk exactly once.
        CrewAffinity.Approve(sarah, 45f);
        check.Equal("reached Admire", AffinityTier.Admire, CrewAffinity.TierOf(sarah));
        check.That("perk unlocked", CrewAffinity.HasPerk(sarah));
        check.Equal("one perk unlock", 1, perkEvents);

        // A later dip below Admire keeps the perk (it stays earned), and climbing back
        // does not re-fire the unlock. From 80, dropping 45 lands at 35 (still Like).
        CrewAffinity.Disapprove(sarah, 45f);
        check.Equal("dipped back to Like", AffinityTier.Like, CrewAffinity.TierOf(sarah));
        check.That("perk stays unlocked through a dip", CrewAffinity.HasPerk(sarah));
        CrewAffinity.Approve(sarah, 60f);
        check.Equal("climbed back to Admire", AffinityTier.Admire, CrewAffinity.TierOf(sarah));
        check.Equal("perk unlock does not re-fire", 1, perkEvents);

        // Disapproval drives the other companion down past Dislike to Hate.
        CrewAffinity.Disapprove(sam, 30f);
        check.Equal("reached Dislike", AffinityTier.Dislike, CrewAffinity.TierOf(sam));
        CrewAffinity.Disapprove(sam, 60f);
        check.Equal("reached Hate", AffinityTier.Hate, CrewAffinity.TierOf(sam));
        check.That("a disliked companion has no perk", !CrewAffinity.HasPerk(sam));

        // The meter is clamped to its floor.
        CrewAffinity.Disapprove(sam, 10000f);
        check.Equal("affinity clamps at the floor", -100f, CrewAffinity.Of(sam));

        CrewAffinity.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
