using Recreation;
using Recreation.Games.Starfield;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Starfield notification layer: it turns the survival and progression
// events into player-facing corner prompts, stays quiet for the events that should
// not prompt (a non-critical hazard stage, oxygen returning to full), and stops
// once the behaviour is removed.
public static class StarfieldNotificationsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        var notifications = new StarfieldNotifications();
        ModHost.Register(notifications);  // DispatchStart subscribes immediately

        EventBus.Publish(new OxygenStageChanged(OxygenStage.Empty));
        EventBus.Publish(new OxygenStageChanged(OxygenStage.Full));        // recovery: quiet
        EventBus.Publish(new CarbonDioxideStageChanged(CarbonDioxideStage.Maxed));
        EventBus.Publish(new AfflictionContracted(fake.Player, "Hypoxia"));
        EventBus.Publish(new HazardStageChanged(SpaceHazard.Radiation, HazardStage.Exposed));   // quiet
        EventBus.Publish(new HazardStageChanged(SpaceHazard.Radiation, HazardStage.Critical));
        EventBus.Publish(new OverMassChanged(true, 120f, 100f));
        EventBus.Publish(new CharacterLeveledUp(fake.Player, 5));
        EventBus.Publish(new SkillRankUp(fake.Player, "Ballistics", 2));
        EventBus.Publish(new ResearchCompleted(fake.Player, "WeaponEngineering"));
        EventBus.Publish(new ItemCrafted(fake.Player, 0x200, 1));

        check.Equal("only the non-quiet events prompt", 9, fake.Notifications.Count);
        check.That("out of oxygen prompted", fake.Notifications.Contains("Out of oxygen"));
        check.That("oxygen recovery stayed quiet", !fake.Notifications.Contains("Oxygen low"));
        check.That("CO2 critical prompted", fake.Notifications.Contains("CO2 critical"));
        check.That("affliction prompted", fake.Notifications.Contains("Afflicted: Hypoxia"));
        check.That("non-critical hazard stayed quiet",
                   fake.Notifications.FindAll(m => m.Contains("hazard")).Count == 1);
        check.That("critical hazard prompted", fake.Notifications.Contains("Radiation hazard critical"));
        check.That("over-mass prompted", fake.Notifications.Contains("Over-encumbered"));
        check.That("level up prompted", fake.Notifications.Contains("Level 5"));
        check.That("skill rank prompted", fake.Notifications.Contains("Ballistics rank 2"));
        check.That("research prompted", fake.Notifications.Contains("Research complete: WeaponEngineering"));
        check.That("craft prompted", fake.Notifications.Contains("Item crafted"));

        // Removing the behaviour disposes its subscriptions, so later events go quiet.
        ModHost.Unregister(notifications);
        EventBus.Publish(new CharacterLeveledUp(fake.Player, 6));
        check.Equal("no prompts after the behaviour is removed", 9, fake.Notifications.Count);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
