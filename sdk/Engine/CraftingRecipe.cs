using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;

namespace Recreation;

// One required input of a recipe: a form and how many of it the recipe consumes.
public readonly struct CraftingInput(Form item, int quantity)
{
    public Form Item { get; } = item;
    public int Quantity { get; } = quantity;
}

// A constructible-object recipe (COBJ): the inputs a crafting station consumes to
// produce an output, tagged with the workbench keyword that station carries.
// Listing recipes for a station and crafting them is generic inventory work, so
// this lives in the SDK; the Skyrim layer only names the stations.
public sealed class CraftingRecipe
{
    public Form Output { get; }
    public int OutputQuantity { get; }
    public Form Workbench { get; }   // the BNAM keyword identifying the station
    public IReadOnlyList<CraftingInput> Inputs { get; }

    public CraftingRecipe(Form output, int outputQuantity, Form workbench,
                          IReadOnlyList<CraftingInput> inputs)
    {
        Output = output;
        OutputQuantity = outputQuantity;
        Workbench = workbench;
        Inputs = inputs;
    }

    // True if `crafter` holds every input in the required quantity.
    public bool CanCraft(ObjectReference crafter) =>
        Inputs.All(i => crafter.GetItemCount(i.Item) >= i.Quantity);

    // Consumes the inputs from `crafter` and gives them the output. Does nothing
    // and returns false if any input is short, so a failed craft never eats items.
    public bool Craft(ObjectReference crafter)
    {
        if (!CanCraft(crafter)) return false;
        foreach (CraftingInput input in Inputs) crafter.RemoveItem(input.Item, input.Quantity);
        crafter.AddItem(Output, OutputQuantity);
        return true;
    }
}

// Reads constructible-object recipes from the engine's record data. Recipes are
// global game data, parsed once by the engine and indexed here on demand.
public static class Recipes
{
    // Every recipe in the load order.
    public static IReadOnlyList<CraftingRecipe> All()
    {
        int count = Call("GetRecipeCount").AsInt();
        var result = new List<CraftingRecipe>(count);
        for (int i = 0; i < count; i++) result.Add(Read(i));
        return result;
    }

    // The recipes crafted at the station carrying `workbench` (its BNAM keyword),
    // e.g. the forge or the cooking pot. Only matching recipes are materialised.
    public static IReadOnlyList<CraftingRecipe> ForWorkbench(Form workbench)
    {
        int count = Call("GetRecipeCount").AsInt();
        var result = new List<CraftingRecipe>();
        for (int i = 0; i < count; i++)
            if (Call("GetNthRecipeWorkbench", i).AsHandle() == workbench.Handle)
                result.Add(Read(i));
        return result;
    }

    private static CraftingRecipe Read(int index)
    {
        var output = Form.From(Call("GetNthRecipeOutput", index).AsHandle());
        int quantity = Call("GetNthRecipeOutputQuantity", index).AsInt();
        var workbench = Form.From(Call("GetNthRecipeWorkbench", index).AsHandle());
        int inputCount = Call("GetNthRecipeInputCount", index).AsInt();
        var inputs = new CraftingInput[inputCount];
        for (int j = 0; j < inputCount; j++)
        {
            var item = Form.From(Call("GetNthRecipeInput", index, j).AsHandle());
            int qty = Call("GetNthRecipeInputQuantity", index, j).AsInt();
            inputs[j] = new CraftingInput(item, qty);
        }
        return new CraftingRecipe(output, quantity, workbench, inputs);
    }

    private static Value Call(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Game", function, args);
}
