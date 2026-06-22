using System;
using Recreation.Games.Starfield;

namespace Recreation.Tests;

// Covers Starfield damage mitigation: a suit's Physical / Energy / EM resistance
// each cuts a hit by a percent that climbs with the rating but stops at the cap,
// and a hit is met by the resistance for its channel.
public static class StarfieldDamageMitigationTests
{
    public static void Run(Check check)
    {
        // 0.05% reduction per resistance point, up to 80%.
        Near(check, "100 resist resists 5%", 5f, DamageMitigation.ResistPercent(100));
        Near(check, "1000 resist resists 50%", 50f, DamageMitigation.ResistPercent(1000));
        check.Equal("resistance caps at 80%", 80f, DamageMitigation.ResistPercent(5000));

        Near(check, "100 resist lets 95% through", 95f, DamageMitigation.After(100, 100));
        Near(check, "1000 resist halves the hit", 50f, DamageMitigation.After(100, 1000));
        Near(check, "capped resist lets 20% through", 20f, DamageMitigation.After(100, 5000));
        check.Equal("no hit, no damage", 0f, DamageMitigation.After(0, 1000));

        // A suit meets each channel with its own rating.
        var suit = new SuitResistance(physical: 1000f, energy: 100f, electromagnetic: 5000f);
        check.Equal("suit gives the physical rating", 1000f,
                    suit.For(StarfieldDamageType.Physical));
        check.Equal("suit gives the energy rating", 100f, suit.For(StarfieldDamageType.Energy));
        check.Equal("suit gives the EM rating", 5000f,
                    suit.For(StarfieldDamageType.Electromagnetic));

        Near(check, "physical hit met by the physical rating", 50f,
             DamageMitigation.After(100, suit, StarfieldDamageType.Physical));
        Near(check, "energy hit met by the energy rating", 95f,
             DamageMitigation.After(100, suit, StarfieldDamageType.Energy));
        Near(check, "EM hit met by the capped EM rating", 20f,
             DamageMitigation.After(100, suit, StarfieldDamageType.Electromagnetic));
    }

    private static void Near(Check check, string name, float expected, float actual) =>
        check.That(name, Math.Abs(expected - actual) < 0.01f);
}
