using System.IO;
using System.Reflection;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers loading mods from a directory: a real load runs the assembly's IMod,
// and a missing directory is a quiet no-op.
public static class ModLoaderTests
{
    public static void Run(Check check)
    {
        ModHost.Shutdown();

        // Loading the directory this test assembly lives in finds it (and the SDK
        // assembly) and runs their mods, including this assembly's SampleMod.
        string dir = Path.GetDirectoryName(typeof(ModLoaderTests).Assembly.Location)!;
        SampleMod.LoadCount = 0;
        int loaded = ModLoader.LoadDirectory(dir);
        check.That("loaded at least one assembly", loaded >= 1);
        check.That("a mod from a loaded assembly ran", SampleMod.LoadCount >= 1);

        ModHost.Shutdown();

        // A missing directory loads nothing and does not throw.
        check.Equal("missing directory loads nothing",
                    0, ModLoader.LoadDirectory(Path.Combine(dir, "does-not-exist")));

        ModHost.Shutdown();
    }
}
