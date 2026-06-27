namespace Recreation.Games.Skyrim;

// A single effect on a brewed potion: the magic effect plus its final magnitude
// and duration, after the brewer's Alchemy skill has scaled the ingredients'
// base values.
public readonly struct PotionEffect(MagicEffect effect, float magnitude, int duration)
{
    public MagicEffect Effect { get; } = effect;
    public float Magnitude { get; } = magnitude;
    public int Duration { get; } = duration;

    // True when the effect harms rather than helps, so a brew led by it is a
    // poison rather than a potion.
    public bool IsHarmful => Effect.IsDetrimental;

    // This effect's contribution to the brew's gold value, from its base cost
    // scaled by the final magnitude and duration.
    public int GoldValue => AlchemyValue.OfEffect(Effect.BaseCost, Magnitude, Duration);
}
