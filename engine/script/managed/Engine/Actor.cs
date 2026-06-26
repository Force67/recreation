using Recreation.Interop;

namespace Recreation;

// A living reference (Papyrus Actor): the player, NPCs and creatures. Adds
// actor values, combat and faction state on top of ObjectReference.
public class Actor : ObjectReference
{
    protected Actor(ulong handle) : base(handle) { }

    public static new Actor From(ulong handle) => new(handle);

    // --- actor values ---------------------------------------------------------
    // The damageable current value, its permanent base, and the current/base
    // ratio (0..1). Names are the Skyrim actor-value ids ("Health", "Magicka",
    // "Stamina", "OneHanded", ...). See the ActorValue constants.
    public float GetValue(string actorValue) => Call("GetActorValue", actorValue).AsFloat();
    public float GetBaseValue(string actorValue) => Call("GetBaseActorValue", actorValue).AsFloat();
    public float GetValuePercent(string actorValue) =>
        Call("GetActorValuePercentage", actorValue).AsFloat();

    public void SetValue(string actorValue, float value) => Call("SetActorValue", actorValue, value);
    public void ForceValue(string actorValue, float value) =>
        Call("ForceActorValue", actorValue, value);
    public void ModValue(string actorValue, float delta) => Call("ModActorValue", actorValue, delta);
    public void DamageValue(string actorValue, float amount) =>
        Call("DamageActorValue", actorValue, amount);
    public void RestoreValue(string actorValue, float amount) =>
        Call("RestoreActorValue", actorValue, amount);

    // Convenience accessors for the three primary attributes.
    public float Health => GetValue(ActorValue.Health);
    public float Magicka => GetValue(ActorValue.Magicka);
    public float Stamina => GetValue(ActorValue.Stamina);

    // --- actor base (NPC_) flags ---------------------------------------------
    // Read through this actor's base object: essentials cannot be killed, unique
    // actors are one of a kind.
    public bool IsEssential => Native.CallMethod(BaseObject.Handle, "IsEssential", default).AsBool();
    public bool IsUnique => Native.CallMethod(BaseObject.Handle, "IsUnique", default).AsBool();

    // --- state ----------------------------------------------------------------
    public int Level => Call("GetLevel").AsInt();
    public bool IsDead => Call("IsDead").AsBool();
    public bool IsInCombat => Call("IsInCombat").AsBool();
    public bool IsSneaking => Call("IsSneaking").AsBool();
    public Actor CombatTarget => From(Call("GetCombatTarget").AsHandle());

    public void EquipItem(Form item) => Call("EquipItem", item);
    public void AddSpell(Form spell) => Call("AddSpell", spell);

    // --- factions -------------------------------------------------------------
    public int GetFactionRank(Faction faction) => Call("GetFactionRank", faction).AsInt();
    public void SetFactionRank(Faction faction, int rank) =>
        Call("SetFactionRank", faction, rank);
    public bool IsInFaction(Faction faction) => Call("IsInFaction", faction).AsBool();
    public void AddToFaction(Faction faction) => Call("AddToFaction", faction);
    public void RemoveFromFaction(Faction faction) => Call("RemoveFromFaction", faction);
}
