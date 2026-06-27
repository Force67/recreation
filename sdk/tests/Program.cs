using System;
using System.Linq;
using System.Reflection;

namespace Recreation.Tests;

// The managed test runner: discovers every suite in the assembly and runs it
// against a shared Check, prints a summary and exits with the failure count so
// CI (and `dotnet run`) gate on it.
//
// A suite is any type named `*Tests` exposing `public static void Run(Check)`.
// Discovery is by reflection (sorted by name for stable output) so a new suite
// drops in by adding a file, with no central list to edit and conflict on.
internal static class Program
{
    private static int Main()
    {
        var check = new Check();

        MethodInfo[] suites = typeof(Program).Assembly.GetTypes()
            .Where(t => t is { IsClass: true, IsAbstract: true, IsSealed: true } && t.Name.EndsWith("Tests"))
            .Select(t => t.GetMethod("Run", BindingFlags.Public | BindingFlags.Static,
                                     new[] { typeof(Check) }))
            .Where(m => m != null)
            .OrderBy(m => m!.DeclaringType!.Name, StringComparer.Ordinal)
            .ToArray()!;

        foreach (MethodInfo suite in suites)
        {
            try
            {
                suite.Invoke(null, new object[] { check });
            }
            catch (Exception e)
            {
                // A suite that throws is one failure, not an aborted run; unwrap the
                // reflection wrapper so the real exception shows.
                check.That($"{suite.DeclaringType!.Name}.Run threw: {(e.InnerException ?? e).Message}", false);
            }
        }

        Console.WriteLine($"[tests] {suites.Length} suites, {check.Passed} passed, {check.Failed} failed");
        return check.Failed;
    }
}
