using System;

namespace Recreation.Tests;

// A tiny assertion harness so the test runner needs no external framework. Each
// test method takes a Check and records pass/fail; the runner reports the total.
public sealed class Check
{
    public int Passed { get; private set; }
    public int Failed { get; private set; }

    public void That(string what, bool ok)
    {
        if (ok)
        {
            Passed++;
        }
        else
        {
            Failed++;
            Console.WriteLine($"  FAIL: {what}");
        }
    }

    public void Equal<T>(string what, T expected, T actual)
    {
        bool ok = Equals(expected, actual);
        if (!ok) Console.WriteLine($"  FAIL: {what} (expected {expected}, got {actual})");
        if (ok) Passed++;
        else Failed++;
    }
}
