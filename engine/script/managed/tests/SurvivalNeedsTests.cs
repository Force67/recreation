using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the survival hunger model: it rises with the game clock, crosses stages
// (raising events), and falls when the player eats food.
public static class SurvivalNeedsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { GameTime = 10.0f };
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        HungerStage announced = HungerStage.Sated;
        int events = 0;
        using var sub = EventBus.Subscribe<HungerStageChanged>(e =>
        {
            announced = e.Stage;
            events++;
        });

        var needs = new SurvivalNeeds
        {
            DrainPerGameHour = 10f,  // 10 hunger per in-game hour
            HungryAt = 50f,
            StarvingAt = 85f,
            RestorePerFood = 30f,
            FoodKeyword = SkyrimForms.VendorItemFood,
        };
        ModHost.Register(needs);

        // Six in-game hours pass (0.25 day): 60 hunger -> Hungry.
        fake.GameTime = 10.25f;
        ModHost.Tick(0.1f);
        check.Equal("hunger accrues with game time", 60f, needs.Hunger);
        check.Equal("crossed into Hungry", HungerStage.Hungry, announced);
        check.Equal("one stage change so far", 1, events);

        // Three more hours: 90 hunger -> Starving.
        fake.GameTime = 10.375f;
        ModHost.Tick(0.1f);
        check.Equal("crossed into Starving", HungerStage.Starving, needs.Stage);
        check.Equal("two stage changes", 2, events);

        // Eating two food items restores 60, dropping back below Hungry.
        const ulong apple = 0x900;
        fake.SetHasKeyword(apple, Game.GetForm(SkyrimForms.VendorItemFood).Handle);
        EventBus.Publish(new ItemAdded(fake.Player, apple, 2));
        check.Equal("eating reduces hunger", 30f, needs.Hunger);
        check.Equal("back to Sated", HungerStage.Sated, needs.Stage);
        check.Equal("three stage changes", 3, events);

        // Non-food items do not feed.
        const ulong sword = 0x901;
        EventBus.Publish(new ItemAdded(fake.Player, sword, 1));
        check.Equal("non-food does not feed", 30f, needs.Hunger);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
