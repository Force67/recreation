using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// One input to a recipe: a resource form id and how many are consumed.
public readonly struct RecipeInput(uint item, int count)
{
    public uint Item { get; } = item;
    public int Count { get; } = count;
}

// A crafting recipe: the output form and count, the research that must be complete
// to use it ("" for none), and the resources it consumes. A managed definition the
// mod or test supplies.
public sealed class Recipe(uint output, int outputCount, string requiredResearch, RecipeInput[] inputs)
{
    public uint Output { get; } = output;
    public int OutputCount { get; } = outputCount;
    public string RequiredResearch { get; } = requiredResearch;
    public RecipeInput[] Inputs { get; } = inputs;
}

// Raised when an actor crafts a recipe's output.
public readonly struct ItemCrafted(ulong actorHandle, uint item, int count) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public uint Item { get; } = item;
    public int Count { get; } = count;
}

// Starfield crafting: a recipe yields its output once the required research is
// complete and the player holds every input, consuming the inputs from their
// inventory. The research gate is itself skill-gated (see Research), so the full
// chain is skill -> research -> recipe.
public static class Crafting
{
    // True when the recipe's research is done and the actor holds every input.
    public static bool CanCraft(Actor actor, Recipe recipe)
    {
        if (recipe.RequiredResearch.Length != 0 && !Research.IsComplete(actor, recipe.RequiredResearch))
            return false;
        foreach (RecipeInput input in recipe.Inputs)
            if (actor.GetItemCount(Game.GetForm(input.Item)) < input.Count) return false;
        return true;
    }

    // Crafts the recipe, consuming its inputs and adding the output. Returns whether
    // it was crafted.
    public static bool Craft(Actor actor, Recipe recipe)
    {
        if (!CanCraft(actor, recipe)) return false;
        foreach (RecipeInput input in recipe.Inputs)
            actor.RemoveItem(Game.GetForm(input.Item), input.Count);
        actor.AddItem(Game.GetForm(recipe.Output), recipe.OutputCount);
        EventBus.Publish(new ItemCrafted(actor.Handle, recipe.Output, recipe.OutputCount));
        return true;
    }
}
