using System;
using System.Collections.Generic;
using System.Linq;
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
    private readonly Dictionary<ulong, int> _sex = new();        // base -> 0/1
    private readonly Dictionary<ulong, ulong> _race = new();     // base -> race form
    private readonly Dictionary<ulong, int> _weaponDamage = new();  // weapon -> damage
    private readonly Dictionary<ulong, float> _armorRating = new();  // armor -> rating
    private readonly Dictionary<ulong, int> _formType = new();       // form -> type code
    private readonly Dictionary<ulong, ulong> _harvest = new();      // flora -> ingredient
    private readonly Dictionary<ulong, ulong> _enchantment = new();  // item -> enchantment
    private readonly HashSet<ulong> _disabled = new();

    public void SetFormType(ulong form, int type) => _formType[form] = type;
    public void SetHarvestIngredient(ulong flora, ulong ingredient) => _harvest[flora] = ingredient;
    public void SetEnchantment(ulong item, ulong enchantment) => _enchantment[item] = enchantment;

    public void SetActorBaseData(ulong baseObject, int sex, ulong race)
    {
        _sex[baseObject] = sex;
        _race[baseObject] = race;
    }

    public void SetWeaponDamage(ulong weapon, int damage) => _weaponDamage[weapon] = damage;
    public void SetArmorRating(ulong armor, float rating) => _armorRating[armor] = rating;
    private readonly HashSet<(ulong, ulong)> _keywords = new();
    private readonly Dictionary<ulong, (float X, float Y, float Z)> _positions = new();
    private readonly Dictionary<ulong, Dictionary<ulong, int>> _inventory = new();  // container -> item -> count
    private readonly Dictionary<ulong, float> _weights = new();  // item -> unit weight

    // Adds items to a container's inventory, recording each form's unit weight.
    public void AddInventoryItem(ulong container, ulong item, int count = 1, float weight = 0f)
    {
        if (!_inventory.TryGetValue(container, out var items))
        {
            items = new Dictionary<ulong, int>();
            _inventory[container] = items;
        }
        items[item] = items.GetValueOrDefault(item) + count;
        _weights[item] = weight;
    }

    // Marks `item` as carrying `keyword`, for HasKeyword.
    public void SetHasKeyword(ulong item, ulong keyword) => _keywords.Add((item, keyword));

    public void SetPosition(ulong reference, float x, float y, float z) =>
        _positions[reference] = (x, y, z);

    public (float X, float Y, float Z) GetPosition(ulong reference) =>
        _positions.TryGetValue(reference, out var p) ? p : (0f, 0f, 0f);

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

    // Mirrors the engine's player-control gate.
    public bool PlayerControlsEnabled { get; private set; } = true;

    // In-game time in days; the fractional part is the time of day.
    public float GameTime { get; set; }

    public Value CallGlobal(string type, string function, ReadOnlySpan<Value> args)
    {
        LastGlobalType = type;
        LastGlobalFunction = function;
        LastStringArg = args.Length > 0 && args[0].Kind == ValueKind.String ? args[0].AsString() : "";
        LastBoolArg = args.Length > 0 && args[0].AsBool();
        if (type == "Game" && function == "GetPlayer") return Value.Object(Player);
        if (type == "Game" && function == "GetForm") return Value.Object((ulong)args[0].AsInt() & 0xFFFFFFFF);
        if (type == "Game" && function == "EnableFastTravel") FastTravelEnabled = LastBoolArg;
        if (type == "Game" && function == "DisablePlayerControls") PlayerControlsEnabled = false;
        if (type == "Game" && function == "EnablePlayerControls") PlayerControlsEnabled = true;
        if (type == "Utility" && function == "GetCurrentGameTime") return Value.Float(GameTime);
        if (type == "Utility" && function == "RandomInt")
            return Value.Int(args.Length > 0 ? args[0].AsInt() : 0);  // deterministic: the min
        if (type == "Utility" && function == "RandomFloat")
            return Value.Float(args.Length > 0 ? args[0].AsFloat() : 0f);  // deterministic: the min
        return Value.None;
    }

    // The last method dispatched and its first argument, for asserting that the
    // thin wrappers call the right native.
    public string LastMethod { get; private set; } = "";
    public Value LastMethodArg0 { get; private set; } = Value.None;

    public Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args)
    {
        LastMethod = function;
        LastMethodArg0 = args.Length > 0 ? args[0] : Value.None;
        switch (function)
        {
            case "GetActorValue":
                return Value.Float(GetCurrent(self, args[0].AsString()));
            case "GetBaseActorValue":
                return Value.Float(_base.TryGetValue((self, Norm(args[0].AsString())), out float b) ? b : 0f);
            case "GetActorValuePercentage":
            {
                string av = Norm(args[0].AsString());
                float bv = _base.TryGetValue((self, av), out float bb) ? bb : 0f;
                if (bv <= 0f) return Value.Float(0f);
                float cv = _current.TryGetValue((self, av), out float cc) ? cc : 0f;
                return Value.Float(System.Math.Clamp(cv / bv, 0f, 1f));
            }
            case "ModActorValue":
            {
                string av = Norm(args[0].AsString());
                _current[(self, av)] = (_current.TryGetValue((self, av), out float c0) ? c0 : 0f) + args[1].AsFloat();
                return Value.None;
            }
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
            case "HasKeyword":
                return Value.Bool(_keywords.Contains((self, args[0].AsHandle())));
            case "GetPositionX":
                return Value.Float(GetPosition(self).X);
            case "GetPositionY":
                return Value.Float(GetPosition(self).Y);
            case "GetPositionZ":
                return Value.Float(GetPosition(self).Z);
            case "SetPosition":
                _positions[self] = (args[0].AsFloat(), args[1].AsFloat(), args[2].AsFloat());
                return Value.None;
            case "GetNumItems":
                return Value.Int(_inventory.TryGetValue(self, out var items) ? items.Count : 0);
            case "GetNthForm":
            {
                int idx = args[0].AsInt();
                return _inventory.TryGetValue(self, out var inv) && idx >= 0 && idx < inv.Count
                    ? Value.Object(inv.Keys.ElementAt(idx))
                    : Value.Object(0);
            }
            case "GetItemCount":
                return Value.Int(_inventory.TryGetValue(self, out var owned) &&
                                 owned.TryGetValue(args[0].AsHandle(), out int n) ? n : 0);
            case "AddItem":
                AddInventoryItem(self, args[0].AsHandle(), args.Length > 1 ? args[1].AsInt() : 1,
                                 _weights.GetValueOrDefault(args[0].AsHandle()));
                return Value.None;
            case "RemoveItem":
            {
                if (_inventory.TryGetValue(self, out var inv2) &&
                    inv2.TryGetValue(args[0].AsHandle(), out int cur))
                {
                    int take = args.Length > 1 ? args[1].AsInt() : 1;
                    int left = cur - take;
                    if (left > 0) inv2[args[0].AsHandle()] = left;
                    else inv2.Remove(args[0].AsHandle());
                }
                return Value.None;
            }
            case "GetWeight":
                return Value.Float(_weights.GetValueOrDefault(self));
            case "GetSex":
                return Value.Int(_sex.GetValueOrDefault(self));
            case "GetRace":
                return Value.Object(_race.GetValueOrDefault(self));
            case "GetWeaponDamage":
                return Value.Int(_weaponDamage.GetValueOrDefault(self));
            case "GetArmorRating":
                return Value.Float(_armorRating.GetValueOrDefault(self));
            case "GetType":
                return Value.Int(_formType.GetValueOrDefault(self));
            case "GetHarvestIngredient":
                return Value.Object(_harvest.GetValueOrDefault(self));
            case "GetEnchantment":
                return Value.Object(_enchantment.GetValueOrDefault(self));
            case "Disable":
                _disabled.Add(self);
                return Value.None;
            case "Enable":
                _disabled.Remove(self);
                return Value.None;
            case "IsEnabled":
                return Value.Bool(!_disabled.Contains(self));
            case "IsDisabled":
                return Value.Bool(_disabled.Contains(self));
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
