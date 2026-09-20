using System;
using System.Collections.Generic;
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

    // Loads the server-streamed client scripts: absolute paths of managed
    // assemblies the joining client has already streamed and cached (and the
    // player has agreed to run). Assemblies load into the engine's own load
    // context so their SDK references bind to the types already in memory, and
    // ModHost filters them by realm like any other mod. The paths may be in any
    // order: a Resolving probe resolves a dependency from the same batch no
    // matter which file names it first.
    //
    // The engine's load context is not collectible, so assemblies live for the
    // process: a file the server re-streams under a new hash keeps its assembly
    // name and is skipped with a note that a reconnect applies it.
    public static int LoadStreamedScripts(IReadOnlyList<string> paths)
    {
        if (paths.Count == 0) return 0;
        EnsureStreamedProbe();
        // Register the whole batch before loading anything, so a dependency
        // inside the batch resolves while its dependent is still loading,
        // whatever order the files come in.
        foreach (string path in paths)
            if (!streamedPaths.Contains(path)) streamedPaths.Add(path);

        int loaded = 0;
        var assemblies = new List<Assembly>();
        foreach (string path in paths)
        {
            string name = Path.GetFileNameWithoutExtension(path);
            if (AlreadyLoaded(path))
            {
                Console.WriteLine(
                    $"[mods] streamed script {name} is already loaded; a reconnect applies server changes to it");
                continue;
            }
            Assembly? assembly = TryLoad(path);
            if (assembly == null) continue;
            assemblies.Add(assembly);
            loaded++;
        }
        if (loaded > 0)
        {
            ModHost.LoadFrom(assemblies);
            Console.WriteLine(
                $"[mods] loaded {loaded} streamed client script(s) from the server");
        }
        return loaded;
    }

    // Registers (once) the Resolving hook that lets streamed assemblies satisfy
    // each other's dependencies regardless of load order: when the runtime asks
    // for an assembly nothing has loaded yet, the probe offers the batch's file
    // whose name matches. Streamed files are content-store blobs on disk, so the
    // probe matches exact recorded paths, not directory listings.
    private static void EnsureStreamedProbe()
    {
        if (probeInstalled) return;
        probeInstalled = true;
        HostContext.Resolving += (context, name) =>
        {
            string? simpleName = name.Name;
            if (string.IsNullOrEmpty(simpleName)) return null;
            foreach (string path in streamedPaths)
            {
                if (path.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(Path.GetFileNameWithoutExtension(path), simpleName,
                                  StringComparison.OrdinalIgnoreCase))
                {
                    return context.LoadFromAssemblyPath(path);
                }
            }
            return null;
        };
    }

    // Every streamed client-script path offered this process, probed for missing
    // dependencies. Entries persist so an assembly can still resolve against the
    // batch it arrived in even after later batches load.
    private static readonly List<string> streamedPaths = new();
    private static bool probeInstalled;

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
