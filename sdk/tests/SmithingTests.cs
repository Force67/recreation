using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the tempering calculator: quality tiers from Smithing skill (doubled by
// the perk), and the improved weapon damage and armor rating it produces.
public static class SmithingTests
{
    public static void Run(Check check)
    {
        var smithing = new Smithing();  // defaults: 20 skill per tier, +25% per tier

        // Tiers scale with skill; the perk doubles them; below a tier gives none.
        check.Equal("five tiers at 100 skill", 5, smithing.Tiers(100f));
        check.Equal("the perk doubles the tiers", 10, smithing.Tiers(100f, hasPerk: true));
        check.Equal("no tiers below the first threshold", 0, smithing.Tiers(10f));

        // Tempered damage: base + base * 0.25 * tiers.
        check.Equal("tempered damage at 100 skill", 225, smithing.TemperedDamage(100, 100f));
        check.Equal("untempered at no skill", 100, smithing.TemperedDamage(100, 0f));
        check.Equal("the perk improves it further", 350, smithing.TemperedDamage(100, 100f, hasPerk: true));

        // Tempered armor: base * (1 + 0.25 * tiers).
        check.Equal("tempered armor at 100 skill", 180f, smithing.TemperedArmor(80f, 100f));

        // Convenience over the records.
        var fake = new FakeBackend();
        Native.Backend = fake;
        const ulong sword = 0x100, cuirass = 0x101;
        fake.SetWeaponDamage(sword, 40);
        fake.SetArmorRating(cuirass, 40f);
        fake.SetValue(fake.Player, ActorValue.Smithing, current: 100, baseValue: 100);

        Actor smith = Game.Player;
        check.Equal("tempers a weapon for a smith", 90, smithing.TemperedDamage(Form.From(sword), smith));
        check.Equal("tempers armor for a smith", 90f, smithing.TemperedArmor(Form.From(cuirass), smith));

        Native.Backend = null;
    }
}
