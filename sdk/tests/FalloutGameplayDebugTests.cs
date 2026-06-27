using System.Linq;
using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the gameplay debug panel: its keypad shortcuts drive the survival and
// progression systems through the same paths real play uses, each reports a
// notification, and the shortcuts go inert once the panel is removed.
public static class FalloutGameplayDebugTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        CharacterProgress.Clear();
        ChemAddiction.Clear();
        FactionReputation.Clear();
        Special.Clear();
        Effects.Clear();
        EventBus.Clear();

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Health, 1000, 1000);

        var radiation = new Radiation { RadsPerSecond = 100f };
        var ap = new ActionPoints { BaseMax = 60f };
        var debug = new FalloutGameplayDebug { XpPerGrant = 100f, ApPerShot = 25f, ReputationPerHelp = 20 };
        ModHost.Register(radiation);
        ModHost.Register(ap);
        ModHost.Register(debug);

        // Num2 grants XP through the real progression system.
        EventBus.Publish(new KeyPressed(Key.Num2));
        check.Equal("debug grant adds XP", 100f, CharacterProgress.Experience(player));
        EventBus.Publish(new KeyPressed(Key.Num2));
        check.Equal("a second grant levels up", 2, CharacterProgress.Level(player));

        // Num1 toggles the rad field on the live radiation system.
        EventBus.Publish(new KeyPressed(Key.Num1));
        check.That("rad field toggled on", radiation.Irradiated);
        ModHost.Tick(2f);
        check.That("rads accumulate while toggled on", radiation.Rads > 0f);
        EventBus.Publish(new KeyPressed(Key.Num1));
        check.That("rad field toggled off", !radiation.Irradiated);

        // Num3 spends action points through the real pool.
        float beforeAp = ap.Current;
        EventBus.Publish(new KeyPressed(Key.Num3));
        check.Equal("debug spent action points", beforeAp - 25f, ap.Current);

        // Num4 cleans up: decontaminate and cure addictions.
        ChemAddiction.Use(player, new Chem("Jet", ActorValue.Stamina, 20f, 1f), roll: 0f);
        check.That("an addiction is carried", ChemAddiction.Count(player) > 0);
        EventBus.Publish(new KeyPressed(Key.Num4));
        check.Equal("rads decontaminated", 0f, radiation.Rads);
        check.Equal("addictions cured", 0, ChemAddiction.Count(player));

        // J raises Minutemen reputation.
        EventBus.Publish(new KeyPressed(Key.J));
        check.Equal("Minutemen reputation raised", 20, FactionReputation.Of(CommonwealthFaction.Minutemen));

        check.That("the panel reported notifications", fake.Notifications.Count > 0);
        check.That("a grant was announced",
                   fake.Notifications.Any(m => m.StartsWith("Debug: +100 XP")));

        // Once removed, the shortcuts no longer fire.
        ModHost.Unregister(debug);
        int repBefore = FactionReputation.Of(CommonwealthFaction.Minutemen);
        EventBus.Publish(new KeyPressed(Key.J));
        check.Equal("shortcuts inert after removal", repBefore,
                    FactionReputation.Of(CommonwealthFaction.Minutemen));

        ModHost.Shutdown();
        CharacterProgress.Clear();
        ChemAddiction.Clear();
        FactionReputation.Clear();
        Special.Clear();
        Effects.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
