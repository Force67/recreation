using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the networked-entity model: the server spawns and owns objects that
// replicate, a granted owner may move its entity while a non-owner is rejected, and
// clients build the same registry purely from replicated entity state.
public static class NetEntitiesTests
{
    private sealed class Recording : IRpcBackend
    {
        public readonly List<(RpcTarget Target, uint Peer, string Name, Value[] Args)> Emits = new();
        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args) =>
            Emits.Add((target, peer, name, args.ToArray()));
        public void Subscribe(string name) { }
    }

    public static void Run(Check check)
    {
        Native.Backend = new FakeBackend();  // Net.* render natives are inert no-ops

        // --- server: spawn replicates and enters the registry ---
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        var spawned = new List<int>();
        var removed = new List<int>();
        EventBus.Subscribe<EntitySpawned>(e => spawned.Add(e.Entity.Id));
        EventBus.Subscribe<EntityRemoved>(e => removed.Add(e.Id));
        Platform.Boot(NetRole.Server);

        NetEntity? barrel = NetEntities.Spawn("clutter/barrel01", new Vector3(10, 0, 5), owner: 0);
        check.That("server spawn returns a handle", barrel != null);
        check.Equal("spawned entity is live", true, barrel!.Live);
        check.Equal("model reads back", "clutter/barrel01", barrel.Model);
        check.Equal("position reads back", 10f, barrel.Position.X);
        check.That("registry holds the entity", NetEntities.Get(barrel.Id) != null);
        check.That("EntitySpawned fired", spawned.Contains(barrel.Id));
        check.That("spawn replicated the model key",
            rec.Emits.Exists(e => e.Name == "sb:set" &&
                                  e.Args[0].AsString() == $"entity:{barrel.Id}" &&
                                  e.Args[1].AsString() == "model"));

        // --- server: assign ownership; the owner client may move it ---
        NetEntities.SetOwner(barrel.Id, 5);
        check.Equal("ownership assigned", 5u, barrel.Owner);
        // Simulate the owner (peer 5) writing the entity's position over the wire.
        Rpc.Dispatch("sb:req", 5u, false,
            new[] { Value.String($"entity:{barrel.Id}"), Value.String("x"), Value.Float(42f) });
        check.Equal("owner's move is accepted", 42f, barrel.Position.X);

        // --- server: a non-owner client's move is rejected ---
        Rpc.Dispatch("sb:req", 8u, false,
            new[] { Value.String($"entity:{barrel.Id}"), Value.String("x"), Value.Float(999f) });
        check.Equal("non-owner move is rejected", 42f, barrel.Position.X);

        // a client control request is honoured by the server
        Rpc.Dispatch("ent:req_control", 8u, false, new[] { Value.Int(barrel.Id) });
        check.Equal("control request transfers ownership", 8u, barrel.Owner);

        // --- server: delete removes it everywhere ---
        int id = barrel.Id;
        NetEntities.Delete(id);
        check.That("deleted entity leaves the registry", NetEntities.Get(id) == null);
        check.That("EntityRemoved fired", removed.Contains(id));

        // --- client: the registry is built from replicated state alone ---
        Rpc.Clear();
        Rpc.Bind(new Recording());
        spawned.Clear();
        removed.Clear();
        Platform.Boot(NetRole.Client);

        // Server replicates a fully formed entity (transform first, model last).
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("entity:7"), Value.String("x"), Value.Float(3f) });
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("entity:7"), Value.String("model"), Value.String("prop/crate") });
        check.That("client registers the replicated entity", NetEntities.Get(7) != null);
        check.Equal("client reads the model", "prop/crate", NetEntities.Get(7)!.Model);
        check.That("client raised EntitySpawned", spawned.Contains(7));

        // a client cannot spawn directly
        check.That("client spawn is a no-op", NetEntities.Spawn("x", Vector3.Zero) == null);

        // server removal replicates the model-key drop -> removed on the client
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("entity:7"), Value.String("model"), Value.None });
        check.That("client removes the entity on model drop", NetEntities.Get(7) == null);
        check.That("client raised EntityRemoved", removed.Contains(7));

        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
