using System;
using System.Collections.Generic;

namespace Recreation.Modding;

// Runs callbacks after a delay or on a repeating interval, the managed analog of
// Unity's Invoke/InvokeRepeating. Mods schedule timed work (a delayed effect, a
// periodic check) without writing their own frame counting; the mod host advances
// the scheduler each tick. Single-threaded with the rest of the managed world.
public static class Scheduler
{
    private static readonly List<ScheduledTask> Tasks = new();
    private static double _clock;

    // Runs callback once, after `seconds`.
    public static ScheduledTask After(float seconds, Action callback) =>
        Add(seconds, 0, callback);

    // Runs callback every `seconds`, starting one interval from now.
    public static ScheduledTask Every(float seconds, Action callback) =>
        Add(seconds, seconds, callback);

    private static ScheduledTask Add(float delay, float interval, Action callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        var task = new ScheduledTask(_clock + delay, interval, callback);
        Tasks.Add(task);
        return task;
    }

    // Advances the clock and fires every due task. A repeating task reschedules
    // for its next interval; a one-shot is removed. A callback may schedule or
    // cancel tasks without disturbing this pass (the due set is snapshotted).
    public static void Advance(float deltaTime)
    {
        if (Tasks.Count == 0) return;
        _clock += deltaTime;
        var due = new List<ScheduledTask>();
        foreach (ScheduledTask t in Tasks)
            if (t.Active && t.Due <= _clock) due.Add(t);

        foreach (ScheduledTask t in due)
        {
            if (!t.Active) continue;
            if (t.Interval > 0) t.Due += t.Interval;
            else t.Cancel();
            t.Fire();
        }
        Tasks.RemoveAll(t => !t.Active);
    }

    public static int PendingCount => Tasks.Count;

    public static void Clear()
    {
        Tasks.Clear();
        _clock = 0;
    }
}

// A handle to one scheduled callback. Cancel to stop it; a one-shot cancels
// itself after firing.
public sealed class ScheduledTask
{
    internal double Due;
    internal readonly float Interval;
    private readonly Action _callback;

    public bool Active { get; private set; } = true;

    internal ScheduledTask(double due, float interval, Action callback)
    {
        Due = due;
        Interval = interval;
        _callback = callback;
    }

    public void Cancel() => Active = false;

    internal void Fire()
    {
        try
        {
            _callback();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[scheduler] task threw: {ex.Message}");
        }
    }
}
