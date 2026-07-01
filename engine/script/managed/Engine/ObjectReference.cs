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
