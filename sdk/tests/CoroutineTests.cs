using System.Collections;
using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers coroutine resumption across frames, the wait instructions, and stopping.
public static class CoroutineTests
{
    public static void Run(Check check)
    {
        Coroutines.Clear();

        var log = new List<int>();
        IEnumerator Routine()
        {
            log.Add(1);
            yield return new WaitForSeconds(1.0f);
            log.Add(2);
            yield return null;            // one frame
            log.Add(3);
            yield return new WaitForFrames(2);
            log.Add(4);
        }

        Coroutines.Start(Routine());

        Coroutines.Advance(0.1f);  // runs to the first yield
        check.That("first segment runs", log.Count == 1 && log[0] == 1);

        Coroutines.Advance(0.5f);  // still waiting (0.6 < 1.0)
        check.Equal("still waiting on seconds", 1, log.Count);

        Coroutines.Advance(0.5f);  // 1.0s elapsed -> resumes, then waits one frame
        check.That("resumes after the wait", log.Count == 2 && log[1] == 2);

        Coroutines.Advance(0.0f);  // the one-frame wait elapses
        check.That("resumes next frame", log.Count == 3 && log[2] == 3);

        Coroutines.Advance(0.0f);  // first of two frames
        check.Equal("waiting on frames", 3, log.Count);
        Coroutines.Advance(0.0f);  // second frame -> resumes and finishes
        check.That("resumes after frame wait", log.Count == 4 && log[3] == 4);

        Coroutines.Advance(0.0f);
        check.Equal("finished coroutine is gone", 0, Coroutines.RunningCount);

        // Stopping a coroutine prevents further steps.
        Coroutines.Clear();
        int ran = 0;
        IEnumerator Forever()
        {
            while (true)
            {
                ran++;
                yield return null;
            }
        }
        Coroutine handle = Coroutines.Start(Forever());
        Coroutines.Advance(0f);
        check.Equal("ran once before stop", 1, ran);
        Coroutines.Stop(handle);
        Coroutines.Advance(0f);
        check.Equal("stopped coroutine does not run", 1, ran);

        Coroutines.Clear();
    }
}
