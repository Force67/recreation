using Recreation.Interop;

namespace Recreation.Net;

// A networked object every player sees (prop, vehicle, dropped item): a handle around
// its replicated state bag (entity:<id>). Server-authoritative; its owner may also move it.
public sealed class NetEntity
{
    // Reserved bag keys. "model" present marks the entity live.
    internal const string ModelKey = "model";
    internal const string OwnerKey = "owner";

    public int Id { get; }

    internal NetEntity(int id) => Id = id;

    public StateBag State => StateBags.Entity((ulong)Id);

    public bool Live => State.Has(ModelKey);

    public string Model => State.Get(ModelKey).AsString();

    // The peer that controls this entity (0 = server-owned, no player controller).
    public uint Owner => (uint)State.Get(OwnerKey).AsInt();

    public Vector3 Position => new(State.Get("x").AsFloat(), State.Get("y").AsFloat(),
                                   State.Get("z").AsFloat());

    public Vector3 Rotation => new(State.Get("rx").AsFloat(), State.Get("ry").AsFloat(),
                                   State.Get("rz").AsFloat());

    // True when the local player owns this entity (may move it).
    public bool OwnedLocally => Owner != 0 && Owner == Players.LocalId;

    // Move the entity. Allowed on the server or the owning client; a non-owner client's
    // write is rejected by the server.
    public void MoveTo(Vector3 pos)
    {
        State.Set("x", pos.X);
        State.Set("y", pos.Y);
        State.Set("z", pos.Z);
    }

    public void RotateTo(Vector3 euler)
    {
        State.Set("rx", euler.X);
        State.Set("ry", euler.Y);
        State.Set("rz", euler.Z);
    }

    public override string ToString() => $"NetEntity({Id}, {Model})";
}
