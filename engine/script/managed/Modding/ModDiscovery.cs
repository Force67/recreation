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
    public static List<Type> FindMods(IEnumerable<Assembly> assemblies) =>
        Concrete(assemblies)
            .Where(t => typeof(IMod).IsAssignableFrom(t))
            .ToList();

    public static List<Type> FindAutoStartBehaviours(IEnumerable<Assembly> assemblies) =>
        Concrete(assemblies)
            .Where(t => typeof(GameBehaviour).IsAssignableFrom(t) &&
                        t.GetCustomAttribute<AutoStartAttribute>() != null)
            .ToList();

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
