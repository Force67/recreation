using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Raised on every machine when a networked entity appears or is removed.
public readonly struct EntitySpawned(NetEntity entity) : IGameEvent
{
    public NetEntity Entity { get; } = entity;
}

public readonly struct EntityRemoved(int id) : IGameEvent
{
    public int Id { get; } = id;
}

// Registry and authority for networked entities. State lives in entity:<id> bags so
// it replicates; the server spawns and assigns ownership, an owner may move its entity.
public static class NetEntities
{
    private const string SpawnNative = "SpawnObject";
    private const string MoveNative = "MoveObject";
    private const string DeleteNative = "DeleteObject";
    private const string ControlRpc = "ent:req_control";  // client -> server: grant me control

    private static readonly Dictionary<int, NetEntity> Cache = new();
    private static readonly HashSet<int> Live = new();
    private static readonly List<IDisposable> Subscriptions = new();
    private static int _nextId = 1;  // server-assigned ids

    public static IReadOnlyCollection<NetEntity> All => Live.Select(Wrap).ToList();

    public static int Count => Live.Count;

    public static NetEntity? Get(int id) => Live.Contains(id) ? Wrap(id) : null;

    // Server: spawn a networked object. Returns its handle, or null on a client.
    public static NetEntity? Spawn(string model, Vector3 pos, Vector3? rotation = null, uint owner = 0)
    {
        if (!Platform.IsServer) return null;
        int id = _nextId++;
        StateBag bag = StateBags.Entity((ulong)id);
        Vector3 rot = rotation ?? Vector3.Zero;
        // Set position/rotation/owner first, then the model key last: the model key
        // is what marks the entity live, so observers see a fully formed entity.
        bag.Set("x", pos.X);
        bag.Set("y", pos.Y);
        bag.Set("z", pos.Z);
        bag.Set("rx", rot.X);
        bag.Set("ry", rot.Y);
        bag.Set("rz", rot.Z);
        bag.Set(NetEntity.OwnerKey, (int)owner);
        bag.Set(NetEntity.ModelKey, model);
        return Wrap(id);
    }

    // Server: remove a networked object (clears its bag, which removes it everywhere).
    public static void Delete(int id)
    {
        if (!Platform.IsServer) return;
        StateBag bag = StateBags.Entity((ulong)id);
        foreach (string key in bag.Keys.ToList()) bag.Remove(key);
    }

    // Server: grant a player control of an entity (it may then move it).
    public static void SetOwner(int id, uint owner)
    {
        if (Platform.IsServer) StateBags.Entity((ulong)id).Set(NetEntity.OwnerKey, (int)owner);
    }

    // Client: ask the server for control of an entity.
    public static void RequestControl(int id) => Rpc.Emit(ControlRpc, Value.Int(id));

    internal static void Bind(NetRole role)
    {
        Reset();
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
        if (role == NetRole.Server)
        {
            // Compose the write policy: keep the default (own player bag) and also
            // admit an owner writing its own entity bag.
            StateBags.SetClientWriteValidator(AllowOwnerWrites);
            Rpc.On(ControlRpc, e =>
            {
                if (e.Args.Length > 0) SetOwner(e.Args[0].AsInt(), e.Sender);
            });
        }
    }

    internal static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        Cache.Clear();
        Live.Clear();
        _nextId = 1;
    }

    // Server write policy: a client may write its own player bag (the default) or an
    // entity bag it owns. Everything else stays server-authoritative.
    private static bool AllowOwnerWrites(uint peer, string bag, string key, Value value)
    {
        if (bag == $"player:{peer}") return true;
        if (TryEntityId(bag, out int id) && Get(id)?.Owner == peer) return true;
        return false;
    }

    // Keep the registry and the rendered objects in step with replicated entity
    // state. Runs on every machine.
    private static void OnBagChange(StateBagChange change)
    {
        if (!TryEntityId(change.Bag.Name, out int id)) return;

        if (change.Key == NetEntity.ModelKey)
        {
            if (!change.Removed && !change.Value.IsNone)
            {
                if (Live.Add(id))
                {
                    NetEntity e = Wrap(id);
                    Render(SpawnNative, e);
                    EventBus.Publish(new EntitySpawned(e));
                }
            }
            else if (Live.Remove(id))
            {
                Native.CallGlobal("Net", DeleteNative, new[] { Value.Int(id) });
                EventBus.Publish(new EntityRemoved(id));
            }
            return;
        }

        // A live entity moved or re-oriented: refresh its transform on the engine.
        if (Live.Contains(id) && change.Key is "x" or "y" or "z" or "rx" or "ry" or "rz")
            Render(MoveNative, Wrap(id));
    }

    private static void Render(string native, NetEntity e)
    {
        Vector3 p = e.Position;
        Vector3 r = e.Rotation;
        Native.CallGlobal("Net", native, new[]
        {
            Value.Int(e.Id), Value.String(e.Model),
            Value.Float(p.X), Value.Float(p.Y), Value.Float(p.Z),
            Value.Float(r.X), Value.Float(r.Y), Value.Float(r.Z),
        });
    }

    private static NetEntity Wrap(int id)
    {
        if (!Cache.TryGetValue(id, out NetEntity? e))
        {
            e = new NetEntity(id);
            Cache[id] = e;
        }
        return e;
    }

    private static bool TryEntityId(string bagName, out int id)
    {
        id = 0;
        const string prefix = "entity:";
        return bagName.StartsWith(prefix, StringComparison.Ordinal) &&
               int.TryParse(bagName.AsSpan(prefix.Length), out id);
    }
}
