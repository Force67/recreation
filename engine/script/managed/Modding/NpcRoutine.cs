using System.Collections.Generic;
using Recreation;

namespace Recreation.Modding;

// What an NPC should be doing at this hour, the Radiant-AI sandbox packages a
// schedule resolves to. It is a state, not a movement: where the NPC ought to be,
// for the AI (or a future C++ pathing layer) to act on.
public enum RoutineState
{
    Sleep,    // in bed for the night
    Eat,      // a meal at home or a tavern
    Work,     // at a job: the shop, the forge, the guard post
    Wander,   // off the clock, idling around town
}

// Raised when an NPC's schedule rolls it into a new state (work begins, bedtime).
// The cue the AI and any "is the shop open?" check hook, so they react to the
// transition rather than polling the hour.
public readonly struct RoutineStateChanged(ulong actorHandle, RoutineState from, RoutineState to)
    : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public RoutineState From { get; } = from;
    public RoutineState To { get; } = to;

    public Actor Actor => Actor.From(ActorHandle);
}

// A daily schedule: the state an NPC is in across the 24-hour clock, authored as a
// list of segments that each begin at a whole hour and run until the next. The
// hours need not be sorted; At resolves the segment in force at any hour, wrapping
// past midnight, so a single Sleep segment at 22:00 covers the small hours too.
public sealed class DailySchedule
{
    private readonly List<(int Hour, RoutineState State)> _segments = new();

    // The fallback when no segment is authored (a bare schedule reads as Wander).
    public RoutineState Default { get; set; } = RoutineState.Wander;

    // Adds a segment that begins at `hour` (0..23). A later add at the same hour
    // wins, so a schedule can be tweaked in place.
    public DailySchedule At(int hour, RoutineState state)
    {
        hour = ((hour % 24) + 24) % 24;
        _segments.RemoveAll(s => s.Hour == hour);
        _segments.Add((hour, state));
        return this;
    }

    // The state in force at `hour`: the segment with the greatest start hour at or
    // before it, wrapping to the last segment of the day for the pre-dawn hours.
    public RoutineState StateAt(int hour)
    {
        if (_segments.Count == 0) return Default;
        hour = ((hour % 24) + 24) % 24;

        (int Hour, RoutineState State) chosen = default;
        bool found = false;
        // The latest segment of the day, for the wrap-around: before the first
        // start hour the NPC is still in yesterday's last state.
        (int Hour, RoutineState State) latest = _segments[0];
        foreach (var seg in _segments)
        {
            if (seg.Hour <= hour && (!found || seg.Hour > chosen.Hour))
            {
                chosen = seg;
                found = true;
            }
            if (seg.Hour > latest.Hour) latest = seg;
        }
        return found ? chosen.State : latest.State;
    }
}

// The per-NPC schedule registry and the behaviour that drives it: assign an NPC a
// DailySchedule, and each in-game hour this recomputes every assigned NPC's
// RoutineState and publishes a RoutineStateChanged on a transition. It computes
// state, it does not move anyone, so the AI reads "the smith should be at the forge
// now" without this touching navigation.
//
// Driven by the clock: it subscribes to GameHourStarted (the engine's once-per-hour
// cue) so it costs nothing between hours. Self-contained runtime state; tests and
// teardown clear it.
public sealed class NpcRoutine : GameBehaviour
{
    private static readonly Dictionary<ulong, DailySchedule> Schedules = new();
    private static readonly Dictionary<ulong, RoutineState> Current = new();

    private EventBus.Subscription? _hourSub;

    // Assigns `actor` a schedule and seeds its current state from the clock without
    // firing a transition (the NPC simply starts in the right state).
    public static void Assign(Actor actor, DailySchedule schedule)
    {
        Schedules[actor.Handle] = schedule;
        Current[actor.Handle] = schedule.StateAt(GameClock.Hour);
    }

    public static bool IsScheduled(Actor actor) => Schedules.ContainsKey(actor.Handle);

    // The NPC's current routine state, defaulting to Wander when it has no
    // schedule (an NPC the system does not manage).
    public static RoutineState StateOf(Actor actor) =>
        Current.TryGetValue(actor.Handle, out RoutineState state) ? state : RoutineState.Wander;

    // The state the schedule would put `actor` in at `hour`, ignoring the cached
    // current state. The pure query the AI uses to look ahead.
    public static RoutineState StateAt(Actor actor, int hour) =>
        Schedules.TryGetValue(actor.Handle, out DailySchedule? schedule)
            ? schedule.StateAt(hour) : RoutineState.Wander;

    public static int Count => Schedules.Count;

    public static void Clear()
    {
        Schedules.Clear();
        Current.Clear();
    }

    protected override void OnStart() =>
        _hourSub = EventBus.Subscribe<GameHourStarted>(e => Advance(e.Hour));

    protected override void OnDestroy()
    {
        _hourSub?.Dispose();
        _hourSub = null;
    }

    // Recomputes every assigned NPC's state for `hour`, firing a transition on each
    // change. Public so a test can drive it without the event.
    public static void Advance(int hour)
    {
        foreach (var kv in Schedules)
        {
            RoutineState next = kv.Value.StateAt(hour);
            RoutineState prev = Current.TryGetValue(kv.Key, out RoutineState c) ? c : next;
            if (next == prev && Current.ContainsKey(kv.Key)) continue;
            Current[kv.Key] = next;
            if (next != prev)
                EventBus.Publish(new RoutineStateChanged(kv.Key, prev, next));
        }
    }
}
