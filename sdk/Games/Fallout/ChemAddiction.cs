using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// A Fallout 4 chem and the withdrawal it inflicts: a stable id, the actor value the
// withdrawal saps and by how much, and the per-use chance of hooking the player.
// Jet saps Action Points, Psycho saps damage resistance, Buffout saps Endurance,
// and so on; the value and magnitude carried here stand in for that table.
public readonly struct Chem(string id, string withdrawalValue, float withdrawalMagnitude,
                            float addictionChance)
{
    public string Id { get; } = id;
    public string WithdrawalValue { get; } = withdrawalValue;
    public float WithdrawalMagnitude { get; } = withdrawalMagnitude;
    public float AddictionChance { get; } = addictionChance;  // 0..1
}

// Raised when the player becomes addicted to a chem (withdrawal begins).
public readonly struct ChemAddicted(ulong actorHandle, string chemId) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public string ChemId { get; } = chemId;
}

// Raised when an addiction is cured, lifting its withdrawal.
public readonly struct AddictionCured(ulong actorHandle, string chemId) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public string ChemId { get; } = chemId;
}

// Fallout 4 chem addiction as managed soft logic, the Fallout twin of Skyrim's
// Diseases. Using a chem can hook the player; once hooked, withdrawal saps an actor
// value through the Effects system for as long as the addiction is carried, exactly
// like a disease ability saps a skill. Addictol or a doctor cures every addiction at
// once; a cure reverts each withdrawal. The roll is exposed as `roll` so a test can
// force the outcome and so a future chem-use hook can pass a real RNG, keeping the
// registry deterministic. Persistent player state, so it outlives the mod-host
// lifecycle; tests clear it.
public static class ChemAddiction
{
    private static readonly Dictionary<ulong, Dictionary<string, Effect>> Carried = new();  // actor -> id -> drain

    // Resolves taking a chem: with probability chem.AddictionChance (decided by
    // `roll`, a 0..1 draw the caller supplies) the player becomes addicted and the
    // withdrawal drain is applied. Returns true when this use hooked the player.
    // A chem the player is already addicted to does not re-hook them.
    public static bool Use(Actor who, Chem chem, float roll)
    {
        if (Has(who, chem.Id)) return false;
        if (roll > chem.AddictionChance) return false;

        if (!Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set))
            Carried[who.Handle] = set = new Dictionary<string, Effect>();
        set[chem.Id] = Effects.Apply(who, chem.WithdrawalValue, -chem.WithdrawalMagnitude,
                                     durationSeconds: 0f);
        EventBus.Publish(new ChemAddicted(who.Handle, chem.Id));
        return true;
    }

    public static bool Has(Actor who, string chemId) =>
        Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set) && set.ContainsKey(chemId);

    public static int Count(Actor who) =>
        Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set) ? set.Count : 0;

    // Cures one addiction by id, reverting just its withdrawal so it composes with
    // any others. Returns whether one was lifted.
    public static bool Cure(Actor who, string chemId)
    {
        if (!Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set)) return false;
        if (!set.Remove(chemId, out Effect? drain)) return false;
        Effects.Remove(drain);
        if (set.Count == 0) Carried.Remove(who.Handle);
        EventBus.Publish(new AddictionCured(who.Handle, chemId));
        return true;
    }

    // Cures every addiction the actor carries (Addictol), reverting each withdrawal.
    // Returns how many were lifted.
    public static int CureAll(Actor who)
    {
        if (!Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set) || set.Count == 0)
            return 0;
        var ids = new List<string>(set.Keys);
        foreach (string id in ids) Cure(who, id);
        return ids.Count;
    }

    public static void Clear() => Carried.Clear();
}
