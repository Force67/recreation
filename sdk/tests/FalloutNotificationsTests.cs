using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Fallout notification layer: it turns the survival and progression
// events into player-facing corner prompts, stays quiet for the events that should
// not prompt (rads clearing back to Clean), and stops once the behaviour is removed.
public static class FalloutNotificationsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        var notifications = new CommonwealthNotifications();
        ModHost.Register(notifications);  // DispatchStart subscribes immediately

        EventBus.Publish(new RadiationStageChanged(RadiationStage.Advanced, 600f));
        EventBus.Publish(new RadiationStageChanged(RadiationStage.Clean, 0f));   // recovery: quiet
        EventBus.Publish(new CharacterLeveledUp(fake.Player, 5));
        EventBus.Publish(new EncumbranceChanged(true, 220f, 200f));
        EventBus.Publish(new FactionStandingChanged(CommonwealthFaction.Minutemen,
                                                    FactionStanding.Allied, 80));
        EventBus.Publish(new ChemAddicted(fake.Player, "Jet"));
        EventBus.Publish(new AddictionCured(fake.Player, "Jet"));
        EventBus.Publish(new AdrenalineChanged(3, 0.15f));

        check.Equal("only the non-quiet events prompt", 7, fake.Notifications.Count);
        check.That("advanced rads prompted",
                   fake.Notifications.Contains("Radiation: advanced sickness"));
        check.That("rads clearing stayed quiet",
                   !fake.Notifications.Exists(m => m.StartsWith("Radiation") && m.Contains("Clean")));
        check.That("level up prompted", fake.Notifications.Contains("Level 5"));
        check.That("over-encumbered prompted", fake.Notifications.Contains("Over-encumbered"));
        check.That("faction standing prompted", fake.Notifications.Contains("Minutemen: Allied"));
        check.That("addiction prompted", fake.Notifications.Contains("Addicted: Jet"));
        check.That("cure prompted", fake.Notifications.Contains("Cured: Jet"));
        check.That("adrenaline prompted", fake.Notifications.Contains("Adrenaline rank 3"));

        // Removing the behaviour disposes its subscriptions, so later events go quiet.
        ModHost.Unregister(notifications);
        EventBus.Publish(new CharacterLeveledUp(fake.Player, 6));
        check.Equal("no prompts after the behaviour is removed", 7, fake.Notifications.Count);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
