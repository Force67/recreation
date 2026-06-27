using System;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the damage-mitigation maths: armor and resistance each cut a hit by a
// percentage that climbs with the value but stops at its cap.
public static class DamageMitigationTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // Armor resists 0.12% per point, up to 80%.
        Near(check, "12 armor points resist 1.44%", 1.44f, DamageMitigation.ArmorResistPercent(12));
        check.Equal("armor resistance caps at 80%", 80f, DamageMitigation.ArmorResistPercent(1000));
        Near(check, "100 armor lets 88% through", 88f, DamageMitigation.AfterArmor(100, 100));
        Near(check, "capped armor lets 20% through", 20f, DamageMitigation.AfterArmor(100, 1000));
        check.Equal("no hit, no damage through", 0f, DamageMitigation.AfterArmor(0, 500));

        // Resistance cuts elemental damage, capped at 85%.
        Near(check, "50% resist halves the hit", 50f, DamageMitigation.AfterResist(100, 50));
        Near(check, "over-cap resist still lets 15% through", 15f, DamageMitigation.AfterResist(100, 90));
        Near(check, "negative resist changes nothing", 100f, DamageMitigation.AfterResist(100, -10));

        // Worn pieces sum to the rating the armor formula reads.
        const ulong cuirass = 0x800, boots = 0x801;
        fake.SetArmorRating(cuirass, 40f);
        fake.SetArmorRating(boots, 60f);
        check.Equal("worn armor ratings sum", 100f,
                    DamageMitigation.TotalArmorRating(new[] { Form.From(cuirass), Form.From(boots) }));

        Native.Backend = null;
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
