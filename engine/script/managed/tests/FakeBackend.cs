using System;
using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation.Tests;

// An in-managed stand-in for the engine, so SDK-backed game logic runs in tests
// without a live bridge. It models just enough state for the Skyrim systems
// under test: a player handle, per-actor actor values (current and base), and a
// combat flag. RestoreActorValue clamps at base, matching the engine.
public sealed class FakeBackend : IEngineBackend
{
    public ulong Player { get; set; } = 0x14;

    private readonly Dictionary<(ulong, string), float> _current = new();
    private readonly Dictionary<(ulong, string), float> _base = new();
    private readonly HashSet<ulong> _inCombat = new();
    private readonly Dictionary<ulong, ulong> _baseObject = new();
    private readonly HashSet<ulong> _essential = new();

    public void SetValue(ulong actor, string av, float current, float baseValue)
    {
        _current[(actor, Norm(av))] = current;
        _base[(actor, Norm(av))] = baseValue;
    }

    // Sets an actor's base object handle and whether that base is essential.
    public void SetBase(ulong actor, ulong baseObject, bool essential)
    {
        _baseObject[actor] = baseObject;
        if (essential) _essential.Add(baseObject);
        else _essential.Remove(baseObject);
    }

    public float GetCurrent(ulong actor, string av) =>
        _current.TryGetValue((actor, Norm(av)), out float v) ? v : 0f;

    public void SetInCombat(ulong actor, bool value)
    {
        if (value) _inCombat.Add(actor);
        else _inCombat.Remove(actor);
    }

    // The last global call seen, for asserting the thin API wrappers dispatch to
    // the right native with the right arguments.
    public string LastGlobalType { get; private set; } = "";
    public string LastGlobalFunction { get; private set; } = "";
    public string LastStringArg { get; private set; } = "";
    public bool LastBoolArg { get; private set; }

    // True until the system toggles it; mirrors the engine's fast-travel gate.
    public bool FastTravelEnabled { get; private set; } = true;

    public Value CallGlobal(string type, string function, ReadOnlySpan<Value> args)
    {
        LastGlobalType = type;
        LastGlobalFunction = function;
        LastStringArg = args.Length > 0 && args[0].Kind == ValueKind.String ? args[0].AsString() : "";
        LastBoolArg = args.Length > 0 && args[0].AsBool();
        if (type == "Game" && function == "GetPlayer") return Value.Object(Player);
        if (type == "Game" && function == "EnableFastTravel") FastTravelEnabled = LastBoolArg;
        if (type == "Utility" && function == "RandomInt")
            return Value.Int(args.Length > 0 ? args[0].AsInt() : 0);  // deterministic: the min
        return Value.None;
    }

    public Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args)
    {
        switch (function)
        {
            case "GetActorValue":
                return Value.Float(GetCurrent(self, args[0].AsString()));
            case "GetBaseActorValue":
                return Value.Float(_base.TryGetValue((self, Norm(args[0].AsString())), out float b) ? b : 0f);
            case "RestoreActorValue":
            {
                string av = Norm(args[0].AsString());
                float cap = _base.TryGetValue((self, av), out float bv) ? bv : float.MaxValue;
                float cur = _current.TryGetValue((self, av), out float cv) ? cv : 0f;
                _current[(self, av)] = MathF.Min(cap, cur + args[1].AsFloat());
                return Value.None;
            }
            case "IsInCombat":
                return Value.Bool(_inCombat.Contains(self));
            case "GetBaseObject":
                return Value.Object(_baseObject.TryGetValue(self, out ulong b2) ? b2 : self);
            case "IsEssential":
                return Value.Bool(_essential.Contains(self));
            default:
                return Value.None;
        }
    }

    public bool IsScriptLoaded(string type) => false;
    public bool LoadScript(string type) => false;
    public ulong CreateInstance(string type) => 0;
    public string TypeOf(ulong handle) => string.Empty;
    public Value GetProperty(ulong self, string name) => Value.None;
    public void SetProperty(ulong self, string name, Value value) { }
    public void Tick(float deltaTime) { }

    private static string Norm(string av) => av.ToLowerInvariant();
}
