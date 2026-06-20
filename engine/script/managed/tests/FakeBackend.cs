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
    private readonly Dictionary<ulong, int> _goldValue = new();   // item -> record value
    public void SetGoldValue(ulong item, int value) => _goldValue[item] = value;
    public void SetWeight(ulong item, float weight) => _weights[item] = weight;
    private readonly Dictionary<ulong, (int Soul, int Capacity)> _soulGems = new();
    public void SetSoulGem(ulong gem, int soul, int capacity) => _soulGems[gem] = (soul, capacity);
    private readonly Dictionary<ulong, ulong> _bookSpell = new();   // book -> taught spell
    private readonly Dictionary<ulong, string> _bookSkill = new();  // book -> taught skill av
    private readonly HashSet<(ulong Actor, ulong Spell)> _spells = new();
    public void SetBookSpell(ulong book, ulong spell) => _bookSpell[book] = spell;
    public void SetBookSkill(ulong book, string skill) => _bookSkill[book] = skill;
    public bool HasSpell(ulong actor, ulong spell) => _spells.Contains((actor, spell));

    // ingredient -> its magic effects, and the cache the Nth-accessors read after
    // a GetIngredientEffectCount, mirroring the engine's parse-once-then-index ABI.
    private readonly Dictionary<ulong, List<(ulong Effect, float Magnitude, int Duration)>>
        _ingredientEffects = new();
    private readonly List<(ulong Effect, float Magnitude, int Duration)> _effectCache = new();

    public void SetIngredientEffects(ulong ingredient,
        params (ulong effect, float magnitude, int duration)[] effects) =>
        _ingredientEffects[ingredient] = effects.Select(e => (e.effect, e.magnitude, e.duration)).ToList();

    // The actor value a magic effect modifies, and whether it is detrimental.
    private readonly Dictionary<ulong, string> _effectAv = new();
    private readonly HashSet<ulong> _effectDetrimental = new();
    private readonly Dictionary<ulong, float> _effectBaseCost = new();
    public void SetMagicEffectInfo(ulong effect, string actorValue, bool detrimental,
                                   float baseCost = 0f)
    {
        _effectAv[effect] = actorValue;
        if (detrimental) _effectDetrimental.Add(effect);
        else _effectDetrimental.Remove(effect);
        _effectBaseCost[effect] = baseCost;
    }

    // A spell's SPIT fields: cost, type, cast type and delivery.
    private readonly Dictionary<ulong, (int Cost, int Type, int CastType, int Delivery)> _spellInfo = new();
    public void SetSpellInfo(ulong spell, int cost, int type = 0, int castType = 1, int delivery = 2) =>
        _spellInfo[spell] = (cost, type, castType, delivery);

    // Constructible-object recipes the global recipe accessors read back.
    private readonly List<(ulong Output, int OutputQty, ulong Workbench,
                           List<(ulong Item, int Qty)> Inputs)> _recipes = new();

    public void AddRecipe(ulong output, int outputQty, ulong workbench,
                          params (ulong item, int qty)[] inputs) =>
        _recipes.Add((output, outputQty, workbench, inputs.Select(i => (i.item, i.qty)).ToList()));

    // Leveled lists, and the single-slot cache the LVLI accessors read after a
    // GetLeveledListCount (mirroring the engine's parse-once-then-index ABI).
    private readonly Dictionary<ulong, (int ChanceNone, int Flags,
                                        List<(ulong Form, int Level, int Count)> Entries)> _leveledLists = new();
    private (int ChanceNone, int Flags, List<(ulong Form, int Level, int Count)> Entries) _leveledCache
        = (0, 0, new());

    public void SetLeveledList(ulong list, int chanceNone, int flags,
                               params (ulong form, int level, int count)[] entries) =>
        _leveledLists[list] = (chanceNone, flags,
                               entries.Select(e => (e.form, e.level, e.count)).ToList());
    private readonly HashSet<(ulong, ulong)> _keywords = new();
    private readonly Dictionary<ulong, (float X, float Y, float Z)> _positions = new();
    private readonly List<(ulong Handle, float Distance)> _nearbyCache = new();
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
    private readonly List<ulong> _keywordCache = new();

    // A race's innate spells, and the cache the accessors read after a count call.
    private readonly Dictionary<ulong, List<ulong>> _raceSpells = new();
    private readonly List<ulong> _raceSpellCache = new();
    public void SetRaceSpells(ulong race, params ulong[] spells) => _raceSpells[race] = spells.ToList();

    // A race's starting skill bonuses (skill name, bonus), and their cache.
    private readonly Dictionary<ulong, List<(string Skill, int Bonus)>> _raceSkills = new();
    private readonly List<(string Skill, int Bonus)> _raceSkillCache = new();
    public void SetRaceSkillBonuses(ulong race, params (string skill, int bonus)[] bonuses) =>
        _raceSkills[race] = bonuses.Select(b => (b.skill, b.bonus)).ToList();

    // An actor's authored factions (faction handle, rank) and the cache the
    // enumeration accessors read after a GetFactionCount.
    private readonly Dictionary<ulong, List<(ulong Faction, int Rank)>> _factions = new();
    private readonly List<(ulong Faction, int Rank)> _factionCache = new();
    public void SetActorFactions(ulong actor, params (ulong faction, int rank)[] factions) =>
        _factions[actor] = factions.Select(f => (f.faction, f.rank)).ToList();

    // References spawned via PlaceAtMe (origin, base form, count), for asserting
    // what a spawn produced.
    private readonly List<(ulong Origin, ulong Base, int Count)> _spawned = new();
    public IReadOnlyList<(ulong Origin, ulong Base, int Count)> Spawned => _spawned;
    private ulong _nextSpawn = 0xF0000;

    // Faction-to-faction combat reactions (0 neutral, 1 enemy, 2 ally, 3 friend).
    private readonly Dictionary<(ulong, ulong), int> _reactions = new();
    public void SetFactionReaction(ulong faction, ulong other, int reaction) =>
        _reactions[(faction, other)] = reaction;

    // Faction DATA flags and per-faction crime gold (bounty).
    private readonly Dictionary<ulong, int> _factionFlags = new();
    private readonly Dictionary<ulong, int> _crimeGold = new();
    public void SetFactionFlags(ulong faction, int flags) => _factionFlags[faction] = flags;

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
        if (type == "Game" && function == "GetRecipeCount") return Value.Int(_recipes.Count);
        if (type == "Game" && function == "GetNthRecipeOutput")
            return Value.Object(_recipes[args[0].AsInt()].Output);
        if (type == "Game" && function == "GetNthRecipeOutputQuantity")
            return Value.Int(_recipes[args[0].AsInt()].OutputQty);
        if (type == "Game" && function == "GetNthRecipeWorkbench")
            return Value.Object(_recipes[args[0].AsInt()].Workbench);
        if (type == "Game" && function == "GetNthRecipeInputCount")
            return Value.Int(_recipes[args[0].AsInt()].Inputs.Count);
        if (type == "Game" && function == "GetNthRecipeInput")
            return Value.Object(_recipes[args[0].AsInt()].Inputs[args[1].AsInt()].Item);
        if (type == "Game" && function == "GetNthRecipeInputQuantity")
            return Value.Int(_recipes[args[0].AsInt()].Inputs[args[1].AsInt()].Qty);
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
            case "DamageActorValue":
            {
                string av = Norm(args[0].AsString());
                float cur = _current.TryGetValue((self, av), out float cv) ? cv : 0f;
                _current[(self, av)] = MathF.Max(0f, cur - args[1].AsFloat());
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
            case "GetReaction":
                return Value.Int(_reactions.GetValueOrDefault((self, args[0].AsHandle())));
            case "GetFactionFlags":
                return Value.Int(_factionFlags.GetValueOrDefault(self));
            case "GetCrimeGold":
                return Value.Int(_crimeGold.GetValueOrDefault(self));
            case "SetCrimeGold":
                _crimeGold[self] = args[0].AsInt();
                return Value.None;
            case "ModCrimeGold":
                _crimeGold[self] = _crimeGold.GetValueOrDefault(self) + args[0].AsInt();
                return Value.None;
            case "GetFactionCount":
                _factionCache.Clear();
                if (_factions.TryGetValue(self, out var facs)) _factionCache.AddRange(facs);
                return Value.Int(_factionCache.Count);
            case "GetNthFaction":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _factionCache.Count
                    ? Value.Object(_factionCache[idx].Faction) : Value.Object(0);
            }
            case "GetNthFactionRank":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _factionCache.Count
                    ? Value.Int(_factionCache[idx].Rank) : Value.Int(0);
            }
            case "GetKeywordCount":
                _keywordCache.Clear();
                _keywordCache.AddRange(_keywords.Where(k => k.Item1 == self).Select(k => k.Item2));
                return Value.Int(_keywordCache.Count);
            case "GetRaceSpellCount":
                _raceSpellCache.Clear();
                if (_raceSpells.TryGetValue(self, out var spells)) _raceSpellCache.AddRange(spells);
                return Value.Int(_raceSpellCache.Count);
            case "GetNthRaceSpell":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _raceSpellCache.Count
                    ? Value.Object(_raceSpellCache[idx]) : Value.Object(0);
            }
            case "GetRaceSkillBonusCount":
                _raceSkillCache.Clear();
                if (_raceSkills.TryGetValue(self, out var bonuses)) _raceSkillCache.AddRange(bonuses);
                return Value.Int(_raceSkillCache.Count);
            case "GetNthRaceSkillBonusSkill":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _raceSkillCache.Count
                    ? Value.String(_raceSkillCache[idx].Skill) : Value.String("");
            }
            case "GetNthRaceSkillBonusValue":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _raceSkillCache.Count
                    ? Value.Int(_raceSkillCache[idx].Bonus) : Value.Int(0);
            }
            case "GetNthKeyword":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _keywordCache.Count
                    ? Value.Object(_keywordCache[idx]) : Value.Object(0);
            }
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
            case "GetNearbyRefs":
            {
                _nearbyCache.Clear();
                if (_positions.TryGetValue(self, out var center))
                {
                    float radius = args[0].AsFloat();
                    float r2 = radius * radius;
                    foreach (var kv in _positions)
                    {
                        if (kv.Key == self) continue;
                        float dx = kv.Value.X - center.X, dy = kv.Value.Y - center.Y,
                              dz = kv.Value.Z - center.Z;
                        float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 <= r2) _nearbyCache.Add((kv.Key, System.MathF.Sqrt(d2)));
                    }
                }
                return Value.Int(_nearbyCache.Count);
            }
            case "GetNthNearbyRef":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _nearbyCache.Count
                    ? Value.Object(_nearbyCache[idx].Handle)
                    : Value.Object(0);
            }
            case "GetNthNearbyDistance":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _nearbyCache.Count
                    ? Value.Float(_nearbyCache[idx].Distance)
                    : Value.Float(0);
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
            case "PlaceAtMe":
            {
                ulong baseForm = args[0].AsHandle();
                int count = args.Length > 1 ? args[1].AsInt() : 1;
                _spawned.Add((self, baseForm, count));
                return Value.Object(_nextSpawn++);  // a fresh spawned-reference handle
            }
            case "GetWeight":
                return Value.Float(_weights.GetValueOrDefault(self));
            case "GetGoldValue":
                return Value.Int(_goldValue.GetValueOrDefault(self));
            case "GetSoulGemSoul":
                return Value.Int(_soulGems.TryGetValue(self, out var sg1) ? sg1.Soul : 0);
            case "GetSoulGemCapacity":
                return Value.Int(_soulGems.TryGetValue(self, out var sg2) ? sg2.Capacity : 0);
            case "GetBookSpell":
                return Value.Object(_bookSpell.GetValueOrDefault(self));
            case "GetBookSkill":
                return Value.String(_bookSkill.GetValueOrDefault(self, ""));
            case "AddSpell":
                _spells.Add((self, args[0].AsHandle()));
                return Value.None;
            case "GetLeveledListCount":
                _leveledCache = _leveledLists.TryGetValue(self, out var ll)
                    ? ll : (0, 0, new List<(ulong, int, int)>());
                return Value.Int(_leveledCache.Entries.Count);
            case "GetLeveledChanceNone":
                return Value.Int(_leveledCache.ChanceNone);
            case "GetLeveledFlags":
                return Value.Int(_leveledCache.Flags);
            case "GetNthLeveledForm":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _leveledCache.Entries.Count
                    ? Value.Object(_leveledCache.Entries[idx].Form) : Value.Object(0);
            }
            case "GetNthLeveledLevel":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _leveledCache.Entries.Count
                    ? Value.Int(_leveledCache.Entries[idx].Level) : Value.Int(0);
            }
            case "GetNthLeveledCount":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _leveledCache.Entries.Count
                    ? Value.Int(_leveledCache.Entries[idx].Count) : Value.Int(0);
            }
            case "GetMagicEffectCount":
                _effectCache.Clear();
                if (_ingredientEffects.TryGetValue(self, out var effs)) _effectCache.AddRange(effs);
                return Value.Int(_effectCache.Count);
            case "GetMagicEffectActorValue":
                return Value.String(_effectAv.GetValueOrDefault(self, ""));
            case "GetMagicEffectDetrimental":
                return Value.Bool(_effectDetrimental.Contains(self));
            case "GetMagicEffectBaseCost":
                return Value.Float(_effectBaseCost.GetValueOrDefault(self));
            case "GetSpellCost":
                return Value.Int(_spellInfo.TryGetValue(self, out var sc) ? sc.Cost : 0);
            case "GetSpellType":
                return Value.Int(_spellInfo.TryGetValue(self, out var st) ? st.Type : 0);
            case "GetSpellCastType":
                return Value.Int(_spellInfo.TryGetValue(self, out var sct) ? sct.CastType : 0);
            case "GetSpellDelivery":
                return Value.Int(_spellInfo.TryGetValue(self, out var sd) ? sd.Delivery : 0);
            case "GetNthMagicEffectId":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _effectCache.Count
                    ? Value.Object(_effectCache[idx].Effect) : Value.Object(0);
            }
            case "GetNthMagicEffectMagnitude":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _effectCache.Count
                    ? Value.Float(_effectCache[idx].Magnitude) : Value.Float(0);
            }
            case "GetNthMagicEffectDuration":
            {
                int idx = args[0].AsInt();
                return idx >= 0 && idx < _effectCache.Count
                    ? Value.Int(_effectCache[idx].Duration) : Value.Int(0);
            }
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
            case "GetName":
                return Value.String(_names.GetValueOrDefault(self, ""));
            case "GetJournalEntry":
                return Value.String(_journal.GetValueOrDefault(self, ""));
            default:
                return Value.None;
        }
    }

    private readonly Dictionary<ulong, string> _types = new();  // handle -> script type
    public void SetType(ulong handle, string type) => _types[handle] = type;

    private readonly Dictionary<ulong, string> _names = new();  // form -> FULL name
    public void SetName(ulong handle, string name) => _names[handle] = name;

    private readonly Dictionary<ulong, string> _journal = new();  // quest -> log entry
    public void SetJournalEntry(ulong quest, string entry) => _journal[quest] = entry;

    public bool IsScriptLoaded(string type) => false;
    public bool LoadScript(string type) => false;
    public ulong CreateInstance(string type) => 0;
    public string TypeOf(ulong handle) => _types.GetValueOrDefault(handle, string.Empty);
    public Value GetProperty(ulong self, string name) => Value.None;
    public void SetProperty(ulong self, string name, Value value) { }
    public void Tick(float deltaTime) { }

    private static string Norm(string av) => av.ToLowerInvariant();
}
