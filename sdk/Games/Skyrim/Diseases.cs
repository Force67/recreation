using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when an actor contracts a disease.
public readonly struct DiseaseContracted(ulong actorHandle, ulong diseaseHandle) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public ulong DiseaseHandle { get; } = diseaseHandle;

    public Spell Disease => Spell.From(DiseaseHandle);
}

// Raised when an actor is cured, carrying how many ailments were lifted.
public readonly struct DiseasesCured(ulong actorHandle, int count) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public int Count { get; } = count;
}

// Skyrim diseases: a contracted ailment is a disease-typed ability whose effects
// sap the victim's values for as long as it is carried (Rockjoint weakens melee,
// Ataxia hurts lockpicking and pickpocketing, ...). Contracting applies the
// ability; a cure -- a shrine blessing or a Cure Disease potion -- lifts every
// disease at once. Built on the ability system; this layer enforces that only
// disease-typed spells count and tracks who carries what. Persistent player state,
// so it outlives the mod-host lifecycle; tests clear it.
public static class Diseases
{
    private static readonly Dictionary<ulong, HashSet<ulong>> Carried = new();  // actor -> diseases

    // Infects `who` with `disease` (a disease-typed spell): applies its effects and
    // records it. Returns false when the spell is not a disease or the actor
    // already carries it.
    public static bool Contract(Actor who, Spell disease)
    {
        if (disease.Type != SpellType.Disease) return false;
        if (!Carried.TryGetValue(who.Handle, out HashSet<ulong>? set))
            Carried[who.Handle] = set = new HashSet<ulong>();
        if (!set.Add(disease.Handle)) return false;

        Abilities.Apply(who, disease);
        EventBus.Publish(new DiseaseContracted(who.Handle, disease.Handle));
        return true;
    }

    public static bool Has(Actor who, Spell disease) =>
        Carried.TryGetValue(who.Handle, out HashSet<ulong>? set) && set.Contains(disease.Handle);

    public static int Count(Actor who) =>
        Carried.TryGetValue(who.Handle, out HashSet<ulong>? set) ? set.Count : 0;

    // Cures `who` of every disease they carry, lifting each ailment's effects.
    // Returns how many were cured.
    public static int Cure(Actor who)
    {
        if (!Carried.Remove(who.Handle, out HashSet<ulong>? set) || set.Count == 0) return 0;
        foreach (ulong disease in set) Abilities.Remove(who, Spell.From(disease));
        EventBus.Publish(new DiseasesCured(who.Handle, set.Count));
        return set.Count;
    }

    public static void Clear() => Carried.Clear();
}
