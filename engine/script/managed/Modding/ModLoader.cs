using System;
using System.IO;
using System.Reflection;

namespace Recreation.Modding;

// Loads user mods from disk, the drop-in path that makes the game moddable
// without rebuilding the engine: put a compiled mod assembly in the mods folder
// and it loads at startup. Each assembly is scanned by the mod host the same way
// the built-in mods are, so an external mod is a first-class citizen.
//
// Mods load into the default context and share the SDK assembly already in
// memory, so a mod's `Game`, `EventBus` and `ModHost` are the engine's. A bad or
// non-managed file is skipped with a warning rather than taking the load down.
public static class ModLoader
{
    // Loads every .dll in directory and registers the mods it declares. Returns
    // the number of assemblies successfully loaded. A missing directory loads
    // nothing.
    public static int LoadDirectory(string directory)
    {
        if (!Directory.Exists(directory))
        {
            Console.WriteLine($"[mods] no mods directory at {directory}");
            return 0;
        }

        int loaded = 0;
        foreach (string path in Directory.EnumerateFiles(directory, "*.dll"))
        {
            Assembly? assembly = TryLoad(path);
            if (assembly == null) continue;
            ModHost.LoadFrom(new[] { assembly });
            loaded++;
        }
        Console.WriteLine($"[mods] loaded {loaded} mod assembl{(loaded == 1 ? "y" : "ies")} from {directory}");
        return loaded;
    }

    private static Assembly? TryLoad(string path)
    {
        try
        {
            return Assembly.LoadFrom(path);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[mods] cannot load {Path.GetFileName(path)}: {ex.Message}");
            return null;
        }
    }
}
