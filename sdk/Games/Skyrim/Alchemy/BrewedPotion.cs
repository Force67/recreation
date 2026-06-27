using System;
using System.Collections.Generic;
using System.Linq;

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

    // The most valuable effect: the one that names the brew and decides whether it
    // is a potion or a poison, the way the game picks the primary effect.
    public PotionEffect PrimaryEffect =>
        Effects.OrderByDescending(e => e.GoldValue).First();

    // A brew is a poison when its primary (most valuable) effect harms; otherwise
    // it is a potion. Mirrors how Skyrim classifies a created mixture.
    public bool IsPoison => IsValid && PrimaryEffect.IsHarmful;
    public bool IsPotion => IsValid && !PrimaryEffect.IsHarmful;

    // The brew's gold value: the sum of its effects' worth.
    public int GoldValue => Effects.Sum(e => e.GoldValue);

    public static BrewedPotion Empty { get; } = new(Array.Empty<PotionEffect>());
}
