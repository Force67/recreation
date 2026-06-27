using System;
using System.Collections.Generic;
using System.Linq;

namespace Recreation;

// Loot queries over a container's inventory: what is worth taking, ordered by
// value or chosen to fit a weight budget. Pure reads over the item records, so a
// mod can build smart auto-looting, follower fetching and merchant valuations.
public static class Loot
{
    // The container's items ordered by unit gold value, most valuable first.
    public static IEnumerable<Form> ByValue(ObjectReference container) =>
        container.Items().OrderByDescending(item => item.GoldValue);

    // The single most valuable item the container holds, or Form.None if empty.
    public static Form MostValuable(ObjectReference container) =>
        ByValue(container).FirstOrDefault() ?? Form.None;

    // The most rewarding haul from `container` that fits within `maxWeight`, taken
    // greedily by value per weight (weightless items first, then the densest). Each
    // chosen item comes with how many to take. A good-enough knapsack for "grab the
    // best loot I can carry".
    public static IReadOnlyList<(Form Item, int Count)> WithinWeight(ObjectReference container,
                                                                     float maxWeight)
    {
        float budget = maxWeight;
        var picks = new List<(Form, int)>();
        foreach (Form item in container.Items().OrderByDescending(Density))
        {
            int held = container.GetItemCount(item);
            float weight = item.Weight;
            int take = weight <= 0f ? held : Math.Min(held, (int)(budget / weight));
            if (take <= 0) continue;
            picks.Add((item, take));
            budget -= take * weight;
        }
        return picks;
    }

    // Gold value per unit of weight; weightless items rank highest.
    private static float Density(Form item) =>
        item.Weight <= 0f ? float.MaxValue : item.GoldValue / item.Weight;
}
