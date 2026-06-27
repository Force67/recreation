using System;

namespace Recreation.Games.Skyrim;

// The gold worth of an alchemical effect, following the magnitude/duration curve
// Skyrim prices a created potion by: an effect is worth its base cost scaled by
// magnitude, and (for a timed effect) by duration, each raised to the 1.1 power
// the game applies. It is the only way to value a player-brewed potion, which has
// no record of its own for the economy to read. A mod that wants a different
// economy supplies its own numbers.
public static class AlchemyValue
{
    // The exponent Skyrim raises magnitude and the duration factor to.
    private const double Exponent = 1.1;

    // Gold value of one effect at the given base cost, magnitude (in its units)
    // and duration (seconds; 0 for an instant effect, which carries no duration
    // factor). Magnitude is floored at 1 so a sub-unit effect still has worth.
    public static int OfEffect(float baseCost, float magnitude, int duration)
    {
        if (baseCost <= 0f) return 0;
        double magFactor = Math.Pow(Math.Max(magnitude, 1f), Exponent);
        double durFactor = duration > 0 ? Math.Pow(duration / 10.0, Exponent) : 1.0;
        return (int)(baseCost * magFactor * durFactor);
    }
}
