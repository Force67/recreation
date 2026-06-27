using System;
using System.Collections.Generic;

namespace Recreation;

// The registry of every game loaded in the process. recreation runs Skyrim and
// Fallout content at the same time; the host hands the managed world one bridge
// per game at boot, and this exposes them so a mod can reach all of them:
//
//     GameWorld fo4 = Domains.Get("Fallout 4");
//     WorldForm gun = fo4.GetForm(0x0004F46A);   // a Fallout form
//     Form sword   = Game.GetForm(0x0001397E);   // the primary (Skyrim) world
//
// Domains.Primary mirrors the static Game/Form/Actor SDK's world. The registry
// is populated by ScriptHost at boot and cleared on shutdown.
public static class Domains
{
    private static readonly List<GameWorld> AllWorlds = new();
    private static readonly Dictionary<string, GameWorld> ByName =
        new(StringComparer.OrdinalIgnoreCase);

    // The primary (rendered) game, the one the static SDK targets. Null before boot.
    public static GameWorld? Primary { get; private set; }

    public static IReadOnlyList<GameWorld> All => AllWorlds;
    public static int Count => AllWorlds.Count;

    // The world with this display name (case-insensitive), or null if not loaded.
    public static GameWorld? Get(string name) =>
        ByName.TryGetValue(name, out GameWorld? world) ? world : null;

    public static bool Has(string name) => ByName.ContainsKey(name);

    internal static void Register(GameWorld world, bool isPrimary)
    {
        AllWorlds.Add(world);
        ByName[world.Name] = world;
        if (isPrimary) Primary = world;
    }

    internal static void Clear()
    {
        AllWorlds.Clear();
        ByName.Clear();
        Primary = null;
    }
}
