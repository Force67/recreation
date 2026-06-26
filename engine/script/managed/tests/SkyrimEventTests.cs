using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Verifies the event-driven Skyrim systems respond to engine events delivered
// through the bus, the gmod-style hook path.
public static class SkyrimEventTests
{
    public static void Run(Check check)
    {
        QuestTracker(check);
        Combat(check);
        Essential(check);
    }

    private static void QuestTracker(Check check)
    {
        ModHost.Shutdown();
        EventBus.Clear();

        var tracker = new QuestProgressTracker();
        ModHost.Register(tracker);
        check.Equal("no stage before any event", -1, tracker.LastStage);

        // The engine delivers events as ManagedEvent payloads; route one through
        // the same translator the host uses.
        EngineEvents.Dispatch(new Interop.ManagedEvent
        {
            Id = Interop.ManagedEventId.QuestStageChanged,
            A = 0xABCD,
            I = 40,
        });
        check.Equal("tracker recorded the stage", 40, tracker.LastStage);
        check.Equal("tracker recorded the quest", 0xABCDUL, tracker.LastQuestHandle);

        // A later stage updates it.
        EngineEvents.Dispatch(new Interop.ManagedEvent
        {
            Id = Interop.ManagedEventId.QuestStageChanged,
            A = 0xABCD,
            I = 50,
        });
        check.Equal("tracker follows progression", 50, tracker.LastStage);

        // Once destroyed it unsubscribes and stops tracking.
        ModHost.Unregister(tracker);
        EngineEvents.Dispatch(new Interop.ManagedEvent
        {
            Id = Interop.ManagedEventId.QuestStageChanged,
            A = 0xABCD,
            I = 60,
        });
        check.Equal("unsubscribed after destroy", 50, tracker.LastStage);

        ModHost.Shutdown();
        EventBus.Clear();
    }

    private static void Combat(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        var tracker = new CombatTracker();
        ModHost.Register(tracker);

        EventBus.Publish(new ActorDied(0x99));
        EventBus.Publish(new ActorDied(0xAB));
        check.Equal("kills counted", 2, tracker.Kills);
        check.Equal("last victim recorded", 0xABUL, tracker.LastVictim.Handle);

        // The player's own death is not a kill.
        EventBus.Publish(new ActorDied(fake.Player));
        check.Equal("player death is not a kill", 2, tracker.Kills);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }

    private static void Essential(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        ModHost.Register(new EssentialProtection { ReviveFraction = 0.25f });

        // An essential actor down at zero health recovers a slice of its base.
        const ulong essential = 0x300;
        fake.SetValue(essential, ActorValue.Health, current: 0, baseValue: 100);
        fake.SetBase(essential, essential, essential: true);
        EventBus.Publish(new ActorDied(essential));
        check.Equal("essential actor revived", 25f, fake.GetCurrent(essential, ActorValue.Health));

        // A non-essential actor stays down.
        const ulong mortal = 0x400;
        fake.SetValue(mortal, ActorValue.Health, current: 0, baseValue: 100);
        fake.SetBase(mortal, mortal, essential: false);
        EventBus.Publish(new ActorDied(mortal));
        check.Equal("mortal actor stays dead", 0f, fake.GetCurrent(mortal, ActorValue.Health));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
