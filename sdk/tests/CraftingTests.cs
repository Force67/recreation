using System.Linq;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the data-driven crafting system: recipes are read from records, filtered
// by their workbench, and crafting consumes the inputs to yield the output as an
// all-or-nothing transaction.
public static class CraftingTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong cookpot = 0x500, forge = 0x501;
        const ulong stew = 0x10, sword = 0x11;
        const ulong cabbage = 0x20, salt = 0x21, ingot = 0x30;

        fake.AddRecipe(stew, 1, cookpot, (cabbage, 2), (salt, 1));
        fake.AddRecipe(sword, 1, forge, (ingot, 3));

        check.Equal("all recipes are read", 2, Recipes.All().Count);

        var cooking = Recipes.ForWorkbench(Form.From(cookpot));
        check.Equal("one recipe at the cooking pot", 1, cooking.Count);
        CraftingRecipe stewRecipe = cooking.Single();
        check.Equal("recipe output is the stew", stew, stewRecipe.Output.Handle);
        check.Equal("recipe has two inputs", 2, stewRecipe.Inputs.Count);

        var crafter = ObjectReference.From(fake.Player);

        // Short an input: cannot craft, and a failed craft eats nothing.
        fake.AddInventoryItem(fake.Player, cabbage, 2);
        check.That("cannot craft without all inputs", !stewRecipe.CanCraft(crafter));
        check.That("failed craft returns false", !stewRecipe.Craft(crafter));
        check.Equal("failed craft consumed nothing", 2, crafter.GetItemCount(Form.From(cabbage)));

        // With every input, crafting consumes them and yields the output.
        fake.AddInventoryItem(fake.Player, salt, 1);
        check.That("can craft with all inputs", stewRecipe.CanCraft(crafter));
        check.That("craft succeeds", stewRecipe.Craft(crafter));
        check.Equal("cabbage consumed", 0, crafter.GetItemCount(Form.From(cabbage)));
        check.Equal("salt consumed", 0, crafter.GetItemCount(Form.From(salt)));
        check.Equal("stew produced", 1, crafter.GetItemCount(Form.From(stew)));

        // The Skyrim station helper resolves a keyword id and filters to it. The
        // fake maps GetForm(id) to the low 32 bits, so a recipe tagged with that
        // handle is found via the named cooking pot.
        fake.AddRecipe(0x12, 1, SkyrimWorkbenches.CookingPot & 0xFFFFFFFF, (cabbage, 1));
        var atCookpot = SkyrimWorkbenches.For(SkyrimWorkbenches.CookingPot);
        check.Equal("named cooking pot finds its recipe", 1, atCookpot.Count);
        check.Equal("its output is right", 0x12UL, atCookpot.Single().Output.Handle);

        Native.Backend = null;
    }
}
