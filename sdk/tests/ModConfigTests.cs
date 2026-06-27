using System.IO;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers per-mod config: fallbacks for a missing file, typed reads, and a
// save/reload round trip.
public static class ModConfigTests
{
    public static void Run(Check check)
    {
        string dir = Path.Combine(Path.GetTempPath(), "rec_modconfig_test");
        if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);

        // A missing file gives an empty config that returns the fallbacks.
        ModConfig fresh = ModConfig.Load("Sample", dir);
        check.Equal("missing float falls back", 1.5f, fresh.GetFloat("rate", 1.5f));
        check.That("missing key not present", !fresh.Has("rate"));

        // Set values, save, reload and read them back with the right types.
        fresh.Set("rate", 0.25f);
        fresh.Set("count", 7);
        fresh.Set("enabled", true);
        fresh.Set("label", "hello");
        fresh.Save();

        ModConfig reloaded = ModConfig.Load("Sample", dir);
        check.Equal("float persisted", 0.25f, reloaded.GetFloat("rate", 0f));
        check.Equal("int persisted", 7, reloaded.GetInt("count", 0));
        check.That("bool persisted", reloaded.GetBool("enabled", false));
        check.Equal("string persisted", "hello", reloaded.GetString("label", ""));
        check.That("key present after save", reloaded.Has("rate"));

        // A wrong-typed read still falls back rather than throwing.
        check.Equal("type mismatch falls back", 9, reloaded.GetInt("label", 9));
        // Reading a non-integer number as an int falls back instead of throwing.
        check.Equal("non-integer number as int falls back", 3, reloaded.GetInt("rate", 3));

        Directory.Delete(dir, recursive: true);
    }
}
