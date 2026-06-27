using System;
using Recreation.Interop;

namespace Recreation;

// One loaded game's content, addressable on its own. recreation can run several
// games at once (Skyrim and Fallout 4 side by side); each is a GameWorld backed
// by its own engine bridge, so a mod can read and drive every game's forms in
// the same session. The static Game/Form/Actor SDK targets the primary
// (rendered) world; reach the others through Domains.
//
// A GameWorld dispatches into its domain's own Papyrus guest and record store,
// so handles from one world are only meaningful through that same world.
public sealed class GameWorld
{
    // The game's display name, e.g. "Skyrim Special Edition" or "Fallout 4".
    public string Name { get; }

    internal IEngineBackend Backend { get; }

    internal GameWorld(string name, IEngineBackend backend)
    {
        Name = name;
        Backend = backend;
    }

    // Raw dispatch into this world, the analog of Native.CallGlobal/CallMethod
    // but scoped to this game. Advanced mods reach any registered native here.
    public Value CallGlobal(string type, string function, params ReadOnlySpan<Value> args) =>
        Backend.CallGlobal(type, function, args);

    public Value CallMethod(ulong handle, string function, params ReadOnlySpan<Value> args) =>
        Backend.CallMethod(handle, function, args);

    // Resolves a runtime (load-order) form id in this world, or WorldForm.None.
    public WorldForm GetForm(uint formId) =>
        new(this, CallGlobal("Game", "GetForm", Value.Int((int)formId)).AsHandle());

    // The player character of this world (may be null/None in a non-rendered
    // domain that has no spawned player).
    public WorldForm Player => new(this, CallGlobal("Game", "GetPlayer").AsHandle());

    public override string ToString() => $"GameWorld({Name})";
}

// A form addressed within a specific GameWorld. A thin handle, like Form, but it
// carries the world it belongs to so its reads route to that game's records,
// letting a mod inspect Skyrim and Fallout forms side by side without the static
// SDK's single-world assumption. Reads mirror the common Form accessors.
public readonly struct WorldForm
{
    public GameWorld World { get; }
    public ulong Handle { get; }

    internal WorldForm(GameWorld world, ulong handle)
    {
        World = world;
        Handle = handle;
    }

    public static readonly WorldForm None = default;

    public bool Exists => Handle != 0 && World != null;

    public uint FormId => (uint)Call("GetFormID").AsInt();
    public string Name => Call("GetName").AsString();
    public int FormType => Call("GetType").AsInt();
    public float Weight => Call("GetWeight").AsFloat();
    public int GoldValue => Call("GetGoldValue").AsInt();
    public int WeaponDamage => Call("GetWeaponDamage").AsInt();
    public float ArmorRating => Call("GetArmorRating").AsFloat();

    private Value Call(string function, params ReadOnlySpan<Value> args) =>
        World.CallMethod(Handle, function, args);

    public override string ToString() => $"WorldForm({World?.Name}, 0x{Handle:X})";
}
