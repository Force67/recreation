using System;
using System.Collections;
using System.Collections.Generic;

namespace Recreation.Modding;

// Unity-style coroutines: run a method that pauses and resumes across frames, so
// a mod writes scripted, timed sequences as straight-line code.
//
//   IEnumerator Intro() {
//       Debug.Notification("Wake up...");
//       yield return new WaitForSeconds(3f);
//       Debug.Notification("...the dragon is here.");
//   }
//   Coroutines.Start(Intro());
//
// `yield return null` waits one frame; WaitForSeconds and WaitForFrames wait a
// duration. The mod host advances every running coroutine each tick.
public static class Coroutines
{
    private static readonly List<Coroutine> Running = new();

    public static Coroutine Start(IEnumerator routine)
    {
        ArgumentNullException.ThrowIfNull(routine);
        var coroutine = new Coroutine(routine);
        Running.Add(coroutine);
        return coroutine;
    }

    public static void Stop(Coroutine coroutine) => coroutine.Stop();

    public static int RunningCount
    {
        get
        {
            int n = 0;
            foreach (Coroutine c in Running)
                if (!c.Done) n++;
            return n;
        }
    }

    // Advances every coroutine by one frame, resuming the ones whose wait elapsed.
    public static void Advance(float deltaTime)
    {
        if (Running.Count == 0) return;
        // Snapshot so a coroutine that starts another does not disturb this pass.
        var snapshot = Running.ToArray();
        foreach (Coroutine c in snapshot) c.Step(deltaTime);
        Running.RemoveAll(c => c.Done);
    }

    public static void Clear() => Running.Clear();
}

// A running coroutine. Created by Coroutines.Start; Done once its routine ends or
// it is stopped.
public sealed class Coroutine
{
    private readonly IEnumerator _routine;
    private YieldInstruction? _waiting;

    public bool Done { get; private set; }

    internal Coroutine(IEnumerator routine) => _routine = routine;

    public void Stop() => Done = true;

    internal void Step(float deltaTime)
    {
        if (Done) return;
        // Still waiting on a yield instruction?
        if (_waiting != null && !_waiting.IsReady(deltaTime)) return;
        _waiting = null;

        bool moved;
        try
        {
            moved = _routine.MoveNext();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[coroutine] threw: {ex.Message}");
            Done = true;
            return;
        }
        if (!moved)
        {
            Done = true;
            return;
        }
        // A YieldInstruction pauses until it is ready; anything else (e.g. null)
        // resumes on the next frame.
        _waiting = _routine.Current as YieldInstruction;
    }
}

// The base for things a coroutine can yield to pause on.
public abstract class YieldInstruction
{
    // Returns true once the wait has elapsed (consuming this frame's delta).
    internal abstract bool IsReady(float deltaTime);
}

// Pauses a coroutine for a number of seconds.
public sealed class WaitForSeconds : YieldInstruction
{
    private float _remaining;

    public WaitForSeconds(float seconds) => _remaining = seconds;

    internal override bool IsReady(float deltaTime)
    {
        _remaining -= deltaTime;
        return _remaining <= 0f;
    }
}

// Pauses a coroutine for a number of frames.
public sealed class WaitForFrames : YieldInstruction
{
    private int _remaining;

    public WaitForFrames(int frames) => _remaining = frames;

    internal override bool IsReady(float deltaTime) => --_remaining <= 0;
}
