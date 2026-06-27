using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// What the player has learned about each ingredient: which of its effect slots
// (0..3) they have discovered, the hidden-until-found knowledge Skyrim tracks per
// ingredient so the alchemy menu shows "?" for the rest. Persistent player state,
// so it outlives the mod-host lifecycle; tests clear it explicitly.
public static class KnownIngredientEffects
{
    private static readonly Dictionary<ulong, HashSet<int>> Known = new();

    // Marks effect slot `index` of `ingredient` discovered; returns true if it was
    // newly learned.
    public static bool Discover(Form ingredient, int index)
    {
        if (index < 0) return false;
        if (!Known.TryGetValue(ingredient.Handle, out HashSet<int>? slots))
            Known[ingredient.Handle] = slots = new HashSet<int>();
        return slots.Add(index);
    }

    public static bool Knows(Form ingredient, int index) =>
        Known.TryGetValue(ingredient.Handle, out HashSet<int>? slots) && slots.Contains(index);

    // How many of the ingredient's effects the player has discovered.
    public static int DiscoveredCount(Form ingredient) =>
        Known.TryGetValue(ingredient.Handle, out HashSet<int>? slots) ? slots.Count : 0;

    // The effects the player knows this ingredient carries, in record order.
    public static IEnumerable<MagicEffectInstance> Of(Ingredient ingredient)
    {
        IReadOnlyList<MagicEffectInstance> effects = ingredient.Effects;
        for (int i = 0; i < effects.Count; i++)
            if (Knows(ingredient, i)) yield return effects[i];
    }

    public static void Clear() => Known.Clear();
}

// Raised when eating an ingredient reveals one of its effects.
public readonly struct IngredientEffectDiscovered(ulong ingredientHandle, int effectIndex) : IGameEvent
{
    public ulong IngredientHandle { get; } = ingredientHandle;
    public int EffectIndex { get; } = effectIndex;

    public Ingredient Ingredient => Recreation.Ingredient.From(IngredientHandle);
}

// The outcome of a taste: whether one was on hand to eat, how many of its effects
// took hold on the eater, and the slot newly revealed (-1 if nothing new).
public readonly struct TasteResult(bool eaten, int effectsApplied, int discovered)
{
    public bool Eaten { get; } = eaten;
    public int EffectsApplied { get; } = effectsApplied;
    public int Discovered { get; } = discovered;
}

// Eating an ingredient, the Skyrim way to sample it: it consumes one from the
// eater, applies its effects to them (an ingredient is a weak potion you can eat),
// and reveals the next of its effects in order, the learn-by-tasting loop. Pure
// soft logic over the record data, the inventory and the knowledge registry.
public static class IngredientTasting
{
    public static TasteResult Eat(Actor who, Ingredient ingredient)
    {
        if (who.GetItemCount(ingredient) <= 0) return new TasteResult(false, 0, -1);
        who.RemoveItem(ingredient, 1);

        IReadOnlyList<MagicEffectInstance> effects = ingredient.Effects;
        int applied = 0;
        foreach (MagicEffectInstance effect in effects)
            if (effect.ApplyTo(who)) applied++;

        // Reveal the first slot not yet known, so repeated tasting walks 0..N in
        // order however the slots were learned (brewing can reveal them too).
        int next = 0;
        while (next < effects.Count && KnownIngredientEffects.Knows(ingredient, next)) next++;
        int discovered = -1;
        if (next < effects.Count)
        {
            KnownIngredientEffects.Discover(ingredient, next);
            discovered = next;
            EventBus.Publish(new IngredientEffectDiscovered(ingredient.Handle, next));
        }
        return new TasteResult(true, applied, discovered);
    }
}
