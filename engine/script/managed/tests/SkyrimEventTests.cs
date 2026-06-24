using Recreation.Games.Skyrim;
using Recreation.Modding;

namespace Recreation.Tests;

// Verifies the event-driven Skyrim systems respond to engine events delivered
// through the bus, the gmod-style hook path.
public static class SkyrimEventTests
{
    public static void Run(Check check)
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
}
