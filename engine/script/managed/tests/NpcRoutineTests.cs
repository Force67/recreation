using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the NPC schedule layer: a DailySchedule resolves the state in force at any
// hour (wrapping past midnight), an assigned NPC's state advances with the clock,
// a transition fires a RoutineStateChanged, and the behaviour drives the advance off
// the hourly clock event.
public static class NpcRoutineTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();
        NpcRoutine.Clear();

        // --- DailySchedule resolution --------------------------------------------
        var smith = new DailySchedule()
            .At(8, RoutineState.Work)
            .At(13, RoutineState.Eat)
            .At(14, RoutineState.Work)
            .At(20, RoutineState.Wander)
            .At(22, RoutineState.Sleep);

        check.Equal("mid-shift is work", RoutineState.Work, smith.StateAt(10));
        check.Equal("lunch is eat", RoutineState.Eat, smith.StateAt(13));
        check.Equal("afternoon is back to work", RoutineState.Work, smith.StateAt(16));
        check.Equal("evening is wander", RoutineState.Wander, smith.StateAt(21));
        check.Equal("night is sleep", RoutineState.Sleep, smith.StateAt(23));
        // Pre-dawn wraps to the last segment of the previous day (still asleep).
        check.Equal("pre-dawn wraps to sleep", RoutineState.Sleep, smith.StateAt(3));
        check.Equal("the wrap also covers midnight", RoutineState.Sleep, smith.StateAt(0));

        // A bare schedule reads as its default.
        var idler = new DailySchedule { Default = RoutineState.Wander };
        check.Equal("a bare schedule is the default", RoutineState.Wander, idler.StateAt(12));

        // --- the registry and behaviour ------------------------------------------
        const ulong actor = 0x40;
        fake.GameTime = 10f / 24f;  // 10:00, the smith is at work
        NpcRoutine.Assign(Actor.From(actor), smith);
        check.That("the NPC is scheduled", NpcRoutine.IsScheduled(Actor.From(actor)));
        check.Equal("the NPC starts in the right state", RoutineState.Work,
                    NpcRoutine.StateOf(Actor.From(actor)));
        check.Equal("look-ahead reads the schedule", RoutineState.Sleep,
                    NpcRoutine.StateAt(Actor.From(actor), 23));

        var changes = new List<RoutineStateChanged>();
        EventBus.Subscribe<RoutineStateChanged>(changes.Add);

        var routine = new NpcRoutine();
        ModHost.Register(routine);

        // The hourly clock event drives the advance; bedtime fires a transition.
        EventBus.Publish(new GameHourStarted(22, 0));
        check.Equal("bedtime advanced the state", RoutineState.Sleep,
                    NpcRoutine.StateOf(Actor.From(actor)));
        check.Equal("a transition fired", 1, changes.Count);
        check.Equal("the transition is to sleep", RoutineState.Sleep, changes[0].To);
        check.Equal("the transition is from work", RoutineState.Work, changes[0].From);
        check.Equal("the transition names the actor", actor, changes[0].ActorHandle);

        // An hour with no state change fires nothing.
        EventBus.Publish(new GameHourStarted(23, 0));  // still asleep
        check.Equal("a no-op hour is silent", 1, changes.Count);

        // Morning rolls the NPC back to work.
        EventBus.Publish(new GameHourStarted(8, 1));
        check.Equal("morning advanced to work", RoutineState.Work,
                    NpcRoutine.StateOf(Actor.From(actor)));
        check.Equal("a second transition fired", 2, changes.Count);

        // An unscheduled actor reads as Wander.
        check.Equal("an unscheduled actor wanders", RoutineState.Wander,
                    NpcRoutine.StateOf(Actor.From(0x99)));

        // Teardown unsubscribes the hourly driver.
        ModHost.Unregister(routine);
        int after = changes.Count;
        EventBus.Publish(new GameHourStarted(13, 1));
        check.Equal("no advance after teardown", after, changes.Count);

        ModHost.Shutdown();
        EventBus.Clear();
        NpcRoutine.Clear();
        Native.Backend = null;
    }
}
