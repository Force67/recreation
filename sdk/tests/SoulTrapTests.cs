using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers soul trapping: a soul goes to the smallest empty gem that can hold it,
// skips full gems, partially fills an oversized gem when that is all there is, and
// is lost when no gem can hold it.
public static class SoulTrapTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong petty = 0x900, common = 0x901, grand = 0x902, grandFull = 0x903;
        fake.SetSoulGem(petty, soul: 0, capacity: (int)SoulSize.Petty);
        fake.SetSoulGem(common, soul: 0, capacity: (int)SoulSize.Common);
        fake.SetSoulGem(grand, soul: 0, capacity: (int)SoulSize.Grand);
        fake.SetSoulGem(grandFull, soul: (int)SoulSize.Grand, capacity: (int)SoulSize.Grand);

        SoulGem[] gems = { SoulGem.From(petty), SoulGem.From(common), SoulGem.From(grand) };

        // A common soul takes the common gem, not the wasteful grand one.
        SoulCapture common3 = SoulTrap.Capture(SoulSize.Common, gems);
        check.That("a common soul is caught", common3.Captured);
        check.Equal("it picks the common gem, not the grand", common, common3.Gem!.Handle);
        check.Equal("it holds a common soul", SoulSize.Common, common3.Soul);

        // A grand soul can only go in the grand gem.
        SoulCapture grand5 = SoulTrap.Capture(SoulSize.Grand, gems);
        check.Equal("a grand soul needs the grand gem", grand, grand5.Gem!.Handle);

        // A petty soul, with only the grand gem free, partially fills it as petty.
        SoulGem[] onlyGrand = { SoulGem.From(grand) };
        SoulCapture petty1 = SoulTrap.Capture(SoulSize.Petty, onlyGrand);
        check.Equal("a petty soul falls to the grand gem", grand, petty1.Gem!.Handle);
        check.Equal("but only fills it to petty", SoulSize.Petty, petty1.Soul);

        // Full gems are skipped; a grand soul with only a full grand gem is lost.
        SoulGem[] onlyFull = { SoulGem.From(grandFull) };
        SoulCapture full = SoulTrap.Capture(SoulSize.Grand, onlyFull);
        check.That("a full gem cannot catch a soul", !full.Captured);
        check.That("and the gem reference is null", full.Gem is null);

        // A soul too big for every empty gem is lost.
        SoulGem[] smallOnly = { SoulGem.From(petty), SoulGem.From(common) };
        SoulCapture tooBig = SoulTrap.Capture(SoulSize.Grand, smallOnly);
        check.That("a grand soul is lost on small gems", !tooBig.Captured);

        Native.Backend = null;
    }
}
