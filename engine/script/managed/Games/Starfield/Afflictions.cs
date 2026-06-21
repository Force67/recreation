using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// A Starfield affliction: a named condition that saps one actor value while the
// victim carries it (hypoxia weakens Health, radiation poisoning would weaken it
// too, ...). Unlike a Skyrim disease this is not a record-backed spell; the cause
// is a managed survival system (OxygenCo2), so the sapped value and its magnitude
// are carried here rather than read from a magic effect.
public readonly struct Affliction(string id, string actorValue, float magnitude)
{
    // A stable key identifying the condition, so the same affliction contracted
    // twice is recognised as one (e.g. "Hypoxia").
    public string Id { get; } = id;
    public string ActorValue { get; } = actorValue;
    public float Magnitude { get; } = magnitude;
}

// Raised when an actor contracts an affliction.
public readonly struct AfflictionContracted(ulong actorHandle, string id) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public string Id { get; } = id;
}

// Raised when an actor is cured, carrying how many afflictions were lifted.
public readonly struct AfflictionsCured(ulong actorHandle, int count) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public int Count { get; } = count;
}

// Starfield afflictions: contracting one fortifies the victim's value by the
// affliction's (negative) magnitude through the Effects system, the same way a
// Skyrim disease ability saps a skill, and a cure lifts every affliction at once
// and reverts its drain. The drain is held for as long as the affliction is
// carried. Persistent player state, so it outlives the mod-host lifecycle; tests
// clear it.
public static class Afflictions
{
    private static readonly Dictionary<ulong, Dictionary<string, Effect>> Carried = new();  // actor -> id -> drain

    // Inflicts `affliction` on `who`: applies its drain and records it. Returns
    // false when the actor already carries an affliction with the same id.
    public static bool Contract(Actor who, Affliction affliction)
    {
        if (!Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set))
            Carried[who.Handle] = set = new Dictionary<string, Effect>();
        if (set.ContainsKey(affliction.Id)) return false;

        set[affliction.Id] = Effects.Apply(who, affliction.ActorValue, -affliction.Magnitude,
                                            durationSeconds: 0f);
        EventBus.Publish(new AfflictionContracted(who.Handle, affliction.Id));
        return true;
    }

    public static bool Has(Actor who, string id) =>
        Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set) && set.ContainsKey(id);

    public static int Count(Actor who) =>
        Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set) ? set.Count : 0;

    // Cures `who` of every affliction they carry, reverting each drain. Returns how
    // many were lifted.
    public static int Cure(Actor who)
    {
        if (!Carried.Remove(who.Handle, out Dictionary<string, Effect>? set) || set.Count == 0) return 0;
        foreach (Effect drain in set.Values) Effects.Remove(drain);
        EventBus.Publish(new AfflictionsCured(who.Handle, set.Count));
        return set.Count;
    }

    // Cures a single affliction by id, reverting just its drain so it composes
    // with any others the actor carries. Returns true when one was lifted.
    public static bool Cure(Actor who, string id)
    {
        if (!Carried.TryGetValue(who.Handle, out Dictionary<string, Effect>? set)) return false;
        if (!set.Remove(id, out Effect? drain)) return false;
        Effects.Remove(drain);
        if (set.Count == 0) Carried.Remove(who.Handle);
        EventBus.Publish(new AfflictionsCured(who.Handle, 1));
        return true;
    }

    public static void Clear() => Carried.Clear();
}
