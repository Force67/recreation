using System.Collections.Generic;
using System.Linq;

namespace Recreation.Games.Skyrim;

// Skyrim alchemy: combining ingredients brews a potion carrying every effect that
// two or more of them share, its potency scaled by the brewer's Alchemy skill.
// Pure managed soft logic over the ingredients' record data, so a mod stands up
// a working crafting station with no engine support, and tunes the strength
// curve by constructing its own.
public sealed class Alchemy
{
    // How much one point of Alchemy raises a shared effect's magnitude. The
    // default gives x1.4 magnitude at 100 skill, the rough vanilla feel before
    // perks.
    public float MagnitudePerSkill { get; init; } = 0.004f;

    // Brews a potion from `ingredients` at the given Alchemy skill. The result
    // carries each effect shared by two or more ingredients, taking the strongest
    // contributor's magnitude and the longest duration, then scaling magnitude by
    // skill. Fewer than two ingredients, or no shared effect, yields an invalid
    // potion (BrewedPotion.IsValid == false).
    public BrewedPotion Combine(IReadOnlyList<Ingredient> ingredients, float alchemySkill)
    {
        if (ingredients.Count < 2) return BrewedPotion.Empty;

        // Tally each effect across the ingredients, counting a given ingredient at
        // most once per effect so a duplicate listing cannot fake a shared effect.
        var byEffect = new Dictionary<ulong, List<IngredientEffect>>();
        foreach (Ingredient ingredient in ingredients)
        {
            var counted = new HashSet<ulong>();
            foreach (IngredientEffect effect in ingredient.Effects)
            {
                if (!counted.Add(effect.Effect.Handle)) continue;
                if (!byEffect.TryGetValue(effect.Effect.Handle, out var occurrences))
                {
                    occurrences = new List<IngredientEffect>();
                    byEffect[effect.Effect.Handle] = occurrences;
                }
                occurrences.Add(effect);
            }
        }

        float scale = 1f + MagnitudePerSkill * alchemySkill;
        var effects = new List<PotionEffect>();
        foreach (List<IngredientEffect> occurrences in byEffect.Values)
        {
            if (occurrences.Count < 2) continue;  // present in one ingredient: not shared
            IngredientEffect strongest = occurrences.OrderByDescending(e => e.Magnitude).First();
            int duration = occurrences.Max(e => e.Duration);
            effects.Add(new PotionEffect(strongest.Effect, strongest.Magnitude * scale, duration));
        }
        return new BrewedPotion(effects);
    }

    // Brews using the brewer's current Alchemy skill.
    public BrewedPotion Combine(Actor brewer, IReadOnlyList<Ingredient> ingredients) =>
        Combine(ingredients, brewer.GetValue(ActorValue.Alchemy));
}
