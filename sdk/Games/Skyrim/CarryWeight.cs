using System;
using System.Collections.Generic;
using System.Linq;

namespace Recreation.Games.Skyrim;

// Carry-weight management: how far over capacity an actor is, and shedding the
// least worthwhile items to get back under it. Built on the loot value-per-weight
// ranking, so it keeps the best haul and drops the dead weight -- the "drop junk
// when over-encumbered" quality-of-life mods do. The Encumbrance system applies
// the penalty; this is the tool to escape it.
public static class CarryWeight
{
    // The actor's carry capacity (the CarryWeight actor value).
    public static float Capacity(Actor actor) => actor.GetValue(ActorValue.CarryWeight);

    // How much weight the actor is over capacity (0 if within it).
    public static float Overflow(Actor actor) => MathF.Max(0f, actor.TotalWeight - Capacity(actor));

    public static bool IsOverEncumbered(Actor actor) => Overflow(actor) > 0f;

    // Removes the least valuable-per-weight items from `actor` until it is within
    // its carry weight, returning what was shed (item and count). The items kept
    // are exactly the most rewarding haul that fits, so good loot is never dropped
    // before junk. A no-op (empty) when the actor is already within capacity.
    public static IReadOnlyList<(Form Item, int Count)> ShedExcess(Actor actor)
    {
        if (!IsOverEncumbered(actor)) return Array.Empty<(Form, int)>();

        // The best haul under capacity is what to keep; the rest is shed.
        Dictionary<ulong, int> keep =
            Loot.WithinWeight(actor, Capacity(actor)).ToDictionary(pick => pick.Item.Handle, pick => pick.Count);

        var shed = new List<(Form, int)>();
        foreach (Form item in actor.Items().ToList())  // snapshot: ShedExcess mutates the inventory
        {
            int drop = actor.GetItemCount(item) - keep.GetValueOrDefault(item.Handle);
            if (drop <= 0) continue;
            actor.RemoveItem(item, drop);
            shed.Add((item, drop));
        }
        return shed;
    }
}
