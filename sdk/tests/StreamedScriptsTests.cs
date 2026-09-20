using System;
using System.IO;
using System.Linq;
using System.Reflection;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the server-streamed client-script loader: assemblies the server offers
// load from arbitrary cache paths in any order, with dependencies supplied by
// the Resolving probe, and already-loaded assemblies are skipped rather than
// split. The fixture assemblies live in their own bin directories (never beside
// the test runner), so a dependency genuinely cannot resolve by default
// probing — only through the loader's streamed-batch probe.
public static class StreamedScriptsTests
{
    public static void Run(Check check)
    {
        ModHost.Shutdown();

        (string? dep, string? usesDep) = FixturePaths(check);
        if (dep == null)
            return;

        // A scratch directory plays the content store. The two assemblies sit in
        // DIFFERENT directories, like content-store shards: default probing can
        // never bridge them, so the loader's probe is the only bridge.
        string cache = Path.Combine(Path.GetTempPath(), "rec_streamed_scripts_test");
        Directory.CreateDirectory(Path.Combine(cache, "a"));
        Directory.CreateDirectory(Path.Combine(cache, "b"));
        string depPath = Path.Combine(cache, "a", "StreamedDepLib.dll");
        string usesDepPath = Path.Combine(cache, "b", "StreamedUsesDep.dll");
        File.Copy(dep, depPath, overwrite: true);
        File.Copy(usesDep, usesDepPath, overwrite: true);

        // The dependent first: it can only finish loading once the probe hands
        // it the dependency from the same streamed batch.
        int loaded = ModLoader.LoadStreamedScripts(new[] { usesDepPath, depPath });
        check.Equal("both streamed assemblies loaded", 2, loaded);

        // The cross-assembly call resolves: the dependent reaches the dependency
        // the probe supplied.
        object? result = InvokeLoaded("StreamedUsesDep.DepCaller", "Call");
        check.Equal("dependent reaches its streamed dependency",
                    "streamed-dep-value", result as string);

        // A re-offer of the same assemblies loads nothing: the engine's load
        // context is not collectible, so a changed assembly applies on the next
        // join rather than splitting identity in this one.
        int reloaded = ModLoader.LoadStreamedScripts(new[] { usesDepPath, depPath });
        check.Equal("already-loaded streamed scripts are skipped", 0, reloaded);

        ModHost.Shutdown();
    }

    // Locates the fixture build outputs. Under ctest the runner runs from the
    // build tree, so the source dir arrives as RECREATION_SDK_SOURCE_DIR; a
    // plain `dotnet run` instead finds the fixtures by walking up to the tests
    // directory. Either way the fixtures live in their own bin directories,
    // never beside the runner.
    private static (string?, string?) FixturePaths(Check check)
    {
        string? sourceDir = Environment.GetEnvironmentVariable("RECREATION_SDK_SOURCE_DIR");
        string? testsRoot = null;
        if (!string.IsNullOrEmpty(sourceDir) &&
            Directory.Exists(Path.Combine(sourceDir, "tests", "fixtures")))
            testsRoot = Path.Combine(sourceDir, "tests");
        if (testsRoot == null)
        {
            string? dir = Path.GetDirectoryName(typeof(StreamedScriptsTests).Assembly.Location);
            for (string? probe = dir; probe != null; probe = Path.GetDirectoryName(probe))
            {
                if (File.Exists(Path.Combine(probe, "fixtures", "StreamedDepLib",
                                             "StreamedDepLib.csproj")))
                {
                    testsRoot = probe;
                    break;
                }
            }
        }
        if (testsRoot == null)
        {
            check.That("streamed-script fixtures found", false);
            return (null, null);
        }
        string? dep = FindBuilt("fixtures/StreamedDepLib", "StreamedDepLib.dll", testsRoot);
        string? usesDep = FindBuilt("fixtures/StreamedUsesDep", "StreamedUsesDep.dll", testsRoot);
        check.That("dependency fixture built", dep != null);
        check.That("dependent fixture built", usesDep != null);
        return (dep, usesDep);
    }

    private static string? FindBuilt(string projectDir, string dll, string testsRoot)
    {
        foreach (string candidate in Directory.EnumerateFiles(
                     Path.Combine(testsRoot, projectDir), dll, SearchOption.AllDirectories))
            return candidate;
        return null;
    }

    private static object? InvokeLoaded(string typeName, string method)
    {
        Assembly assembly = AppDomain.CurrentDomain.GetAssemblies()
            .First(a => a.GetName().Name == typeName.Split('.')[0]);
        return assembly.GetType(typeName)!.GetMethod(method)!.Invoke(null, null);
    }
}
