using Recreation.Modding;

namespace Recreation.Tests;

// Covers delayed and repeating callbacks and cancellation.
public static class SchedulerTests
{
    public static void Run(Check check)
    {
        Scheduler.Clear();

        int fired = 0;
        Scheduler.After(0.5f, () => fired++);
        Scheduler.Advance(0.4f);
        check.Equal("not fired before the delay", 0, fired);
        Scheduler.Advance(0.2f);
        check.Equal("fired once at the delay", 1, fired);
        Scheduler.Advance(1.0f);
        check.Equal("one-shot does not repeat", 1, fired);

        Scheduler.Clear();
        int ticks = 0;
        Scheduler.Every(1.0f, () => ticks++);
        Scheduler.Advance(1.0f);
        Scheduler.Advance(1.0f);
        Scheduler.Advance(1.0f);
        check.Equal("repeating fires each interval", 3, ticks);

        Scheduler.Clear();
        int cancelled = 0;
        ScheduledTask task = Scheduler.After(1.0f, () => cancelled++);
        task.Cancel();
        Scheduler.Advance(2.0f);
        check.Equal("cancelled task does not fire", 0, cancelled);

        // A repeating task can cancel itself from inside its callback.
        Scheduler.Clear();
        int once = 0;
        ScheduledTask? selfStop = null;
        selfStop = Scheduler.Every(1.0f, () =>
        {
            once++;
            selfStop!.Cancel();
        });
        Scheduler.Advance(1.0f);
        Scheduler.Advance(1.0f);
        check.Equal("self-cancelling task fires once", 1, once);
        check.Equal("no pending tasks remain", 0, Scheduler.PendingCount);

        Scheduler.Clear();
    }
}
