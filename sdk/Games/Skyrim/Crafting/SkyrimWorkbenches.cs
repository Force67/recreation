using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// Well-known Skyrim.esm crafting-station keywords: the BNAM a COBJ recipe is
// tagged with. Resolve one with Game.GetForm and pass it to Recipes.ForWorkbench
// to list what the station makes, or use For() below. Base-game ids (load order
// 00), verified against Skyrim SE record data.
public static class SkyrimWorkbenches
{
    public const uint Forge = 0x00088105;       // CraftingSmithingForge
    public const uint Grindstone = 0x00088108;  // CraftingSmithingSharpeningWheel
    public const uint ArmorTable = 0x000ADB78;  // CraftingSmithingArmorTable
    public const uint Skyforge = 0x000F46CE;    // CraftingSmithingSkyforge
    public const uint CookingPot = 0x000A5CB3;  // CraftingCookpot
    public const uint TanningRack = 0x0007866A; // CraftingTanningRack
    public const uint Smelter = 0x000A5CCE;     // CraftingSmelter

    // The recipes crafted at `workbenchKeyword` (a constant above), resolving the
    // keyword id to its form first.
    public static IReadOnlyList<CraftingRecipe> For(uint workbenchKeyword) =>
        Recipes.ForWorkbench(Game.GetForm(workbenchKeyword));
}
