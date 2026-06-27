using System;

namespace Recreation.Modding;

// The entry point a mod implements. The mod host finds every IMod across the
// loaded assemblies, instantiates it, and calls OnLoad once at boot. OnLoad is
// where a mod wires itself up: register behaviours, subscribe to events, read
// config. Keep it light; per-frame work belongs in a GameBehaviour.
public interface IMod
{
    void OnLoad();
}

// Optional metadata on an IMod, surfaced in logs and (later) a mod manager.
// Purely descriptive; a mod without it still loads.
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ModAttribute : Attribute
{
    public string Name { get; }
    public string Author { get; init; } = "";
    public string Version { get; init; } = "1.0.0";

    public ModAttribute(string name) => Name = name;
}

// Marks a GameBehaviour the host should instantiate and start automatically at
// boot, so a mod that is just one behaviour needs no IMod boilerplate. The type
// must have a public parameterless constructor.
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AutoStartAttribute : Attribute
{
}
