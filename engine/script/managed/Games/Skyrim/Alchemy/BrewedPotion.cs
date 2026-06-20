using System;
using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// The result of combining ingredients: every effect the potion carries (each one
// shared by two or more of the ingredients). A combine that finds no shared
// effect yields an invalid potion, the mortar's way of rejecting the mix.
public sealed class BrewedPotion
{
    public IReadOnlyList<PotionEffect> Effects { get; }

    public BrewedPotion(IReadOnlyList<PotionEffect> effects) => Effects = effects;

    // A potion needs at least one shared effect to brew.
    public bool IsValid => Effects.Count > 0;

    public static BrewedPotion Empty { get; } = new(Array.Empty<PotionEffect>());
}
