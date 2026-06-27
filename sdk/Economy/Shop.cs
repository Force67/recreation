using System;
using System.Collections.Generic;

namespace Recreation.Net;

// A vendor: a name and a fixed price list keyed by item id.
public sealed class Shop
{
    // Case-insensitive item lookup.
    private readonly Dictionary<string, int> _items = new(StringComparer.OrdinalIgnoreCase);

    public string Name { get; }

    public Shop(string name, params (string Item, int Price)[] items)
    {
        Name = name;
        foreach ((string item, int price) in items) _items[item] = price;
    }

    public IReadOnlyDictionary<string, int> Items => _items;

    public void SetItem(string item, int price) => _items[item] = price;

    // The price of an item, or null when the shop does not stock it.
    public int? PriceOf(string item) => _items.TryGetValue(item, out int price) ? price : null;
}
