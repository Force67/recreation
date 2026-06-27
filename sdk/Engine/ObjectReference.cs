using System;
using System.Linq;
using Recreation.Interop;

namespace Recreation;

// A placed instance of a form in the world (Papyrus ObjectReference): it has a
// transform, an inventory and an enabled/lock state. Doors, containers, items
// on the ground and actors are all object references.
public class ObjectReference : Form
{
    protected ObjectReference(ulong handle) : base(handle) { }

    public static new ObjectReference From(ulong handle) => new(handle);

    public Vector3 Position
    {
        get => new(Call("GetPositionX").AsFloat(), Call("GetPositionY").AsFloat(),
                   Call("GetPositionZ").AsFloat());
        set => Call("SetPosition", value.X, value.Y, value.Z);
    }

    public float DistanceTo(ObjectReference other) => Call("GetDistance", other).AsFloat();

    // The references within `radius` game units, with their live distance, from
    // the engine's per-frame position snapshot. The basis for area effects,
    // ambushes and awareness; empty when this reference is not in the streamed
    // world. Materialised eagerly, since the engine caches the result between the
    // count and the reads. The distance is the live one, so mods can sort or pick
    // the nearest without mixing in the authored DistanceTo.
    public NearbyRef[] RefsNear(float radius)
    {
        int count = Call("GetNearbyRefs", radius).AsInt();
        var result = new NearbyRef[count];
        for (int i = 0; i < count; i++)
        {
            var reference = From(Call("GetNthNearbyRef", i).AsHandle());
            float distance = Call("GetNthNearbyDistance", i).AsFloat();
            result[i] = new NearbyRef(reference, distance);
        }
        return result;
    }

    // The unit direction from this reference to another, in world space.
    public Vector3 DirectionTo(ObjectReference other) => (other.Position - Position).Normalized;

    // Shifts this reference by a world-space offset.
    public void Translate(Vector3 offset) => Position += offset;

    // The base form this reference is an instance of (its BaseObject).
    public Form BaseObject => Form.From(Call("GetBaseObject").AsHandle());

    public void MoveTo(ObjectReference target) => Call("MoveTo", target);

    public bool Enabled
    {
        get => Call("IsEnabled").AsBool();
        set
        {
            if (value) Call("Enable");
            else Call("Disable");
        }
    }

    public bool IsDisabled => Call("IsDisabled").AsBool();
    public bool Is3DLoaded => Call("Is3DLoaded").AsBool();

    public float Scale
    {
        get => Call("GetScale").AsFloat();
        set => Call("SetScale", value);
    }

    // --- inventory ------------------------------------------------------------
    public int GetItemCount(Form item) => Call("GetItemCount", item).AsInt();
    public void AddItem(Form item, int count = 1) => Call("AddItem", item, count);
    public void RemoveItem(Form item, int count = 1) => Call("RemoveItem", item, count);

    // The number of distinct item forms held.
    public int ItemCount => Call("GetNumItems").AsInt();

    // Enumerates the distinct item forms held. The listing is stable only while
    // the inventory is unchanged, so do not add or remove items while iterating.
    public System.Collections.Generic.IEnumerable<Form> Items()
    {
        int count = ItemCount;
        for (int i = 0; i < count; i++)
            yield return Form.From(Call("GetNthForm", i).AsHandle());
    }

    // The total gold value of everything held (each item's value times how many).
    public int TotalValue
    {
        get
        {
            int total = 0;
            foreach (Form item in Items()) total += item.GoldValue * GetItemCount(item);
            return total;
        }
    }

    // The total weight carried (each item's weight times how many).
    public float TotalWeight
    {
        get
        {
            float total = 0f;
            foreach (Form item in Items()) total += item.Weight * GetItemCount(item);
            return total;
        }
    }

    // Moves up to `count` of `item` into `destination` (a negative count moves
    // all held). The basis for loot and container-management mods.
    public void TransferItemTo(ObjectReference destination, Form item, int count = -1)
    {
        int held = GetItemCount(item);
        int n = count < 0 ? held : Math.Min(count, held);
        if (n <= 0) return;
        RemoveItem(item, n);
        destination.AddItem(item, n);
    }

    // Moves the entire inventory into `destination` (loot-all). The item list is
    // snapshotted first, since the transfer mutates this container.
    public void TransferAllItemsTo(ObjectReference destination)
    {
        foreach (Form item in Items().ToList())
            TransferItemTo(destination, item);
    }

    // --- world ----------------------------------------------------------------
    public void Activate(ObjectReference activator) => Call("Activate", activator);
    public void Delete() => Call("Delete");

    // Spawns `count` of `base` at this reference and returns the new instance.
    public ObjectReference PlaceAtMe(Form baseForm, int count = 1) =>
        From(Call("PlaceAtMe", baseForm, count).AsHandle());

    // --- locks and doors ------------------------------------------------------
    public bool IsLocked => Call("IsLocked").AsBool();
    public int LockLevel
    {
        get => Call("GetLockLevel").AsInt();
        set => Call("SetLockLevel", value);
    }
    public void Lock(bool locked = true) => Call("Lock", locked);

    // Door open state: 0 unknown, 1 open, 2 opening, 3 closed, 4 closing.
    public int OpenState => Call("GetOpenState").AsInt();
    public void SetOpen(bool open) => Call("SetOpen", open);
}
