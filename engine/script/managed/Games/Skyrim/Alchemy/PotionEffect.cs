namespace Recreation.Games.Skyrim;

// A single effect on a brewed potion: the magic effect plus its final magnitude
// and duration, after the brewer's Alchemy skill has scaled the ingredients'
// base values.
public readonly struct PotionEffect(MagicEffect effect, float magnitude, int duration)
{
    public MagicEffect Effect { get; } = effect;
    public float Magnitude { get; } = magnitude;
    public int Duration { get; } = duration;
}
