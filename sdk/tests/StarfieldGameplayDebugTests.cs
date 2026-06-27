using System.Linq;
using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the gameplay debug panel: its keypad shortcuts drive the survival and
// progression systems through the same paths real play uses, each reports a
// notification, and the shortcuts go inert once the panel is removed.
public static class StarfieldGameplayDebugTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        CharacterProgress.Clear();
        Afflictions.Clear();
        EventBus.Clear();

        Actor player = Game.Player;

        var oxygen = new OxygenCo2 { HypoxiaAfterSeconds = 1000f };
        var hazards = new EnvironmentalHazards();
        var rested = new WellRested();
        var debug = new StarfieldGameplayDebug { XpPerGrant = 100f };
        ModHost.Register(oxygen);
        ModHost.Register(hazards);
        ModHost.Register(rested);
        ModHost.Register(debug);

        // Num3 grants XP through the real progression system.
        EventBus.Publish(new KeyPressed(Key.Num3));
        check.Equal("debug grant adds XP", 100f, CharacterProgress.Experience(player));
        EventBus.Publish(new KeyPressed(Key.Num3));
        check.Equal("a second grant levels up", 2, CharacterProgress.Level(player));

        // Num1 toggles exertion on the live oxygen system.
        EventBus.Publish(new KeyPressed(Key.Num1));
        check.That("exertion toggled on", oxygen.Exerting);
        EventBus.Publish(new KeyPressed(Key.Num1));
        check.That("exertion toggled off", !oxygen.Exerting);

        // Num2 toggles a hazard field.
        EventBus.Publish(new KeyPressed(Key.Num2));
        check.Equal("hazard toggled on", SpaceHazard.Radiation, hazards.Hazard);
        EventBus.Publish(new KeyPressed(Key.Num2));
        check.Equal("hazard toggled off", SpaceHazard.None, hazards.Hazard);

        // Num4 clears afflictions.
        Afflictions.Contract(player, OxygenCo2.Hypoxia);
        check.Equal("an affliction is carried", 1, Afflictions.Count(player));
        EventBus.Publish(new KeyPressed(Key.Num4));
        check.Equal("afflictions cleared", 0, Afflictions.Count(player));

        // J rests through the WellRested system.
        EventBus.Publish(new KeyPressed(Key.J));
        check.That("the rest shortcut rested the player", rested.Rested);

        check.That("the panel reported notifications", fake.Notifications.Count > 0);
        check.That("a grant was announced",
                   fake.Notifications.Any(m => m.StartsWith("Debug: +100 XP")));

        // Once removed, the shortcuts no longer fire.
        ModHost.Unregister(debug);
        SpaceHazard before = hazards.Hazard;
        EventBus.Publish(new KeyPressed(Key.Num2));
        check.Equal("shortcuts inert after removal", before, hazards.Hazard);

        ModHost.Shutdown();
        CharacterProgress.Clear();
        Afflictions.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
