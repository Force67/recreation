using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers Starfield research and crafting: research is gated behind a skill rank, a
// recipe needs its research done and its resources in the pack, and crafting
// consumes the inputs and yields the output. Chain: skill -> research -> recipe.
public static class StarfieldCraftingTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        CharacterProgress.Clear();
        Skills.Clear();
        Research.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        var weaponEngineering = new ResearchProject("WeaponEngineering", "Ballistics", 1);
        // Resource handles, resolved the way crafting resolves them so the inventory
        // the system reads matches what the test stocks.
        ulong steel = Game.GetForm(0xA0).Handle;
        ulong tungsten = Game.GetForm(0xA1).Handle;
        var recipe = new Recipe(0x200, 1, "WeaponEngineering",
                                new[] { new RecipeInput(0xA0, 3), new RecipeInput(0xA1, 2) });

        // Research is gated by the Ballistics skill: locked at rank 0.
        check.That("cannot research without the skill rank", !Research.CanResearch(player, weaponEngineering));
        check.That("research fails while gated", !Research.Complete(player, weaponEngineering));

        // Earn a level, unlock Ballistics to rank 1, then the gate opens.
        CharacterProgress.GainXp(player, 175f);
        check.That("unlocked Ballistics", Skills.Unlock(player, "Ballistics"));
        check.That("can research at the required rank", Research.CanResearch(player, weaponEngineering));

        int researched = 0, crafted = 0;
        using var s1 = EventBus.Subscribe<ResearchCompleted>(_ => researched++);
        using var s2 = EventBus.Subscribe<ItemCrafted>(_ => crafted++);

        check.That("completed the research", Research.Complete(player, weaponEngineering));
        check.That("research now shows complete", Research.IsComplete(player, "WeaponEngineering"));
        check.That("completing again is a no-op", !Research.Complete(player, weaponEngineering));
        check.Equal("one research event", 1, researched);

        // Research done but no resources: still cannot craft.
        check.That("cannot craft without resources", !Crafting.CanCraft(player, recipe));

        // Stock only part of the inputs: still short.
        fake.AddInventoryItem(player.Handle, steel, 3);
        check.That("cannot craft with a missing input", !Crafting.CanCraft(player, recipe));

        // Full inputs: now craftable.
        fake.AddInventoryItem(player.Handle, tungsten, 2);
        check.That("can craft with research and resources", Crafting.CanCraft(player, recipe));

        check.That("crafted the recipe", Crafting.Craft(player, recipe));
        check.Equal("steel consumed", 0, player.GetItemCount(Game.GetForm(0xA0)));
        check.Equal("tungsten consumed", 0, player.GetItemCount(Game.GetForm(0xA1)));
        check.Equal("output produced", 1, player.GetItemCount(Game.GetForm(0x200)));
        check.Equal("one craft event", 1, crafted);

        // Inputs gone: cannot craft again.
        check.That("cannot craft once resources are spent", !Crafting.Craft(player, recipe));

        // A research-free recipe with no inputs crafts unconditionally.
        var freebie = new Recipe(0x201, 2, "", []);
        check.That("a research-free recipe crafts on its own", Crafting.Craft(player, freebie));
        check.Equal("its output produced", 2, player.GetItemCount(Game.GetForm(0x201)));

        CharacterProgress.Clear();
        Skills.Clear();
        Research.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
