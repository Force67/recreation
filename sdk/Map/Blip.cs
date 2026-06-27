using Recreation;
using Recreation.Interop;

namespace Recreation.Net;

// The icon a blip draws on the compass, minimap and world. An unknown value falls
// back to Default.
public enum BlipSprite
{
    Default = 0,
    Player,
    Friend,
    Enemy,
    Shop,
    Quest,
    Objective,
    Marker,
}

// A marker on the map, addressed by a string id. A thin handle over the state bag
// named "blip:<id>", so a shared blip replicates and a local blip stays put. Removal
// is just emptying the bag.
public sealed class Blip
{
    internal const string BagPrefix = "blip:";

    // The bag keys that make up a blip. Position is three scalars so each axis
    // replicates independently.
    internal const string KeyX = "x";
    internal const string KeyY = "y";
    internal const string KeyZ = "z";
    internal const string KeyLabel = "label";
    internal const string KeySprite = "sprite";
    internal const string KeyColor = "color";
    internal const string KeyShort = "short";

    // Whether this handle's writes replicate. Decided once, when the registry hands
    // out the handle.
    private readonly bool _replicated;

    public string Id { get; }

    internal Blip(string id, bool replicated)
    {
        Id = id;
        _replicated = replicated;
    }

    internal static string BagNameFor(string id) => BagPrefix + id;

    private StateBag Bag => StateBags.Bag(BagNameFor(Id));

    // Each setter writes the bag; the registry's change observer does the re-render.
    public Vector3 Position
    {
        get
        {
            StateBag bag = Bag;
            return new Vector3(bag.Get(KeyX).AsFloat(), bag.Get(KeyY).AsFloat(), bag.Get(KeyZ).AsFloat());
        }
        set
        {
            StateBag bag = Bag;
            bag.Set(KeyX, value.X, _replicated);
            bag.Set(KeyY, value.Y, _replicated);
            bag.Set(KeyZ, value.Z, _replicated);
        }
    }

    public string Label
    {
        get => Bag.Get(KeyLabel).AsString();
        set => Bag.Set(KeyLabel, value, _replicated);
    }

    public BlipSprite Sprite
    {
        get => (BlipSprite)Bag.Get(KeySprite).AsInt();
        set => Bag.Set(KeySprite, (int)value, _replicated);
    }

    // Packed rgba8. Value has no unsigned slot, so it rides the int bits unchanged.
    public uint Color
    {
        get => unchecked((uint)Bag.Get(KeyColor).AsInt());
        set => Bag.Set(KeyColor, Value.Int(unchecked((int)value)), _replicated);
    }

    // A short-range blip hides from the world map once the player is far away.
    public bool ShortRange
    {
        get => Bag.Get(KeyShort).AsBool();
        set => Bag.Set(KeyShort, value, _replicated);
    }
}
