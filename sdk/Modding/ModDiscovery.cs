using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;

namespace Recreation.Modding;

// Finds the mod surface in a set of assemblies: the IMod entry points and the
// auto-start behaviours. Kept apart from the host so the scanning rules have one
// home and can be tested without booting anything. A candidate type must be a
// concrete public class with a public parameterless constructor.
internal static class ModDiscovery
{
    // The engine's role, mirror of the handshake realm field.
    public const int HostServer = 0;
    public const int HostClient = 1;
    public const int HostStandalone = 2;

    public static List<Type> FindMods(IEnumerable<Assembly> assemblies, int hostRealm) =>
        Concrete(assemblies)
            .Where(t => typeof(IMod).IsAssignableFrom(t) && RunsInRealm(t, hostRealm))
            .ToList();

    public static List<Type> FindAutoStartBehaviours(IEnumerable<Assembly> assemblies, int hostRealm) =>
        Concrete(assemblies)
            .Where(t => typeof(GameBehaviour).IsAssignableFrom(t) &&
                        t.GetCustomAttribute<AutoStartAttribute>() != null &&
                        RunsInRealm(t, hostRealm))
            .ToList();

    // A type's declared realm, Server when it carries no [Realm] (so existing
    // gameplay mods stay host-only).
    public static ModRealm RealmOf(Type type) =>
        type.GetCustomAttribute<RealmAttribute>()?.Realm ?? ModRealm.Server;

    // Standalone runs everything; otherwise a type runs when it is Shared or its
    // realm matches the host's role.
    public static bool RunsInRealm(Type type, int hostRealm)
    {
        if (hostRealm == HostStandalone) return true;
        ModRealm realm = RealmOf(type);
        return realm == ModRealm.Shared || (int)realm == hostRealm;
    }

    private static IEnumerable<Type> Concrete(IEnumerable<Assembly> assemblies) =>
        assemblies.SelectMany(SafeTypes)
            .Where(t => t is { IsClass: true, IsAbstract: false, IsPublic: true } &&
                        t.GetConstructor(Type.EmptyTypes) != null);

    // A partially loaded assembly can throw on GetTypes; salvage what loaded.
    private static IEnumerable<Type> SafeTypes(Assembly assembly)
    {
        try
        {
            return assembly.GetTypes();
        }
        catch (ReflectionTypeLoadException ex)
        {
            return ex.Types.Where(t => t != null)!;
        }
    }
}
