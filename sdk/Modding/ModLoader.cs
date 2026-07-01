using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;

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

    // Loads every .dll in directory into the default context WITHOUT discovering its
    // mods, so a later ModHost.Boot scan finds them exactly like a built-in. Use
    // this for first-party content (the default gamemodes); use LoadDirectory for
    // drop-in user mods after boot. A missing directory loads nothing.
    public static int PreloadDirectory(string directory)
    {
        if (!Directory.Exists(directory)) return 0;
        int loaded = 0;
        foreach (string path in Directory.EnumerateFiles(directory, "*.dll"))
        {
            // Skip anything already in memory (e.g. a stray SDK copy beside the
            // gamemodes): a duplicate splits assembly identity and cross-references
            // stop matching. Drop-in mods go through LoadDirectory, which does not
            // skip, so a re-scanned mod still re-registers.
            if (AlreadyLoaded(path)) continue;
            if (TryLoad(path) != null) loaded++;
        }
        if (loaded > 0)
            Console.WriteLine($"[mods] preloaded {loaded} assembl{(loaded == 1 ? "y" : "ies")} from {directory}");
        return loaded;
    }

    private static bool AlreadyLoaded(string path)
    {
        string name = Path.GetFileNameWithoutExtension(path);
        return AppDomain.CurrentDomain.GetAssemblies()
            .Any(a => string.Equals(a.GetName().Name, name, StringComparison.OrdinalIgnoreCase));
    }

    // Preloads the optional default gamemodes (the per-game rulesets) from the
    // gamemodes/ directory beside the SDK assembly, unless RECREATION_NO_GAMEMODES is
    // set; RECREATION_GAMEMODES_DIR overrides the location. Call before ModHost.Boot
    // so the rulesets boot like the built-ins they replaced.
    public static int PreloadDefaultGamemodes()
    {
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("RECREATION_NO_GAMEMODES")))
        {
            Console.WriteLine("[mods] default gamemodes disabled (RECREATION_NO_GAMEMODES)");
            return 0;
        }
        string dir = Environment.GetEnvironmentVariable("RECREATION_GAMEMODES_DIR") ?? DefaultGamemodesDir();
        return PreloadDirectory(dir);
    }

    // gamemodes/ beside the loaded SDK assembly (build output dir), falling back to
    // the app base directory when the assembly has no on-disk location.
    private static string DefaultGamemodesDir()
    {
        string? baseDir = Path.GetDirectoryName(typeof(ModLoader).Assembly.Location);
        if (string.IsNullOrEmpty(baseDir)) baseDir = AppContext.BaseDirectory;
        return Path.Combine(baseDir, "gamemodes");
    }

    // The load context the SDK itself lives in. The engine's CLR host loads the
    // SDK through hostfxr's load_assembly_and_get_function_pointer, which places it
    // in an isolated component context, NOT AssemblyLoadContext.Default. A mod must
    // load into that same context so its Recreation.Scripting reference binds to the
    // SDK already in memory (its IMod, Game and ModHost are the engine's) instead of
    // failing to resolve or pulling in a duplicate that splits type identity.
    private static readonly AssemblyLoadContext HostContext =
        AssemblyLoadContext.GetLoadContext(typeof(ModLoader).Assembly) ?? AssemblyLoadContext.Default;

    private static Assembly? TryLoad(string path)
    {
        try
        {
            return HostContext.LoadFromAssemblyPath(Path.GetFullPath(path));
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[mods] cannot load {Path.GetFileName(path)}: {ex.Message}");
            return null;
        }
    }
}
