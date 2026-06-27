using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// Which gem catches a trapped soul, and the soul it ends up holding. When nothing
// can catch it (every gem is full or too small) the soul is lost: Captured is
// false and Gem is null.
public readonly struct SoulCapture(bool captured, SoulGem? gem, SoulSize soul)
{
    public bool Captured { get; } = captured;
    public SoulGem? Gem { get; } = gem;
    public SoulSize Soul { get; } = soul;

    public static SoulCapture Lost { get; } = new(false, null, SoulSize.None);
}

// Soul trapping: choosing which of the caster's soul gems catches a slain
// creature's soul. The rule is Skyrim's -- the soul goes to the smallest empty gem
// that can hold it, so a grand gem is not wasted on a petty soul, and a soul too
// big for every gem is lost. This is the decision; the caller performs the fill,
// since filling swaps the empty gem for its filled variant (a different form), and
// the caller holds the inventory. Pure soft logic over the gems' record state.
public static class SoulTrap
{
    // Picks the gem that catches a soul of `soulSize` from `gems`: the smallest
    // empty gem whose capacity can hold it. A petty soul dropped into the only
    // (larger) gem available fills it only to petty.
    public static SoulCapture Capture(SoulSize soulSize, IEnumerable<SoulGem> gems)
    {
        if (soulSize == SoulSize.None) return SoulCapture.Lost;

        SoulGem? best = null;
        foreach (SoulGem gem in gems)
        {
            if (gem.IsFilled || gem.Capacity < soulSize) continue;
            if (best == null || gem.Capacity < best.Capacity) best = gem;
        }
        return best == null ? SoulCapture.Lost : new SoulCapture(true, best, soulSize);
    }
}
