using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the map subsystem across roles: shared blips broadcast, replicated blips render, local blips stay local, and the waypoint round-trips.
public static class MapTests
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
        Native.Backend = new FakeBackend();
        var fake = (FakeBackend)Native.Backend!;

        // Replicate one blip-bag key, as the server's authoritative echo would.
        static void ServerSets(string bag, string key, Value value) =>
            Rpc.Dispatch("sb:set", 0u, true, new[] { Value.String(bag), Value.String(key), value });

        // --- server: a shared blip is authored, replicates, updates and removes ---
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Server);
        Map.Bind(NetRole.Server);

        Blip? created = Blips.CreateShared("q1", new Vector3(1, 2, 3), "Quest", BlipSprite.Quest);
        check.That("server CreateShared returns a handle", created != null);
        check.That("shared blip is live", Blips.Get("q1") != null);
        check.Equal("registry holds one blip", 1, Blips.All.Count);

        Blip q1 = Blips.Get("q1")!;
        check.Equal("blip label reads back", "Quest", q1.Label);
        check.Equal("blip sprite reads back", BlipSprite.Quest, q1.Sprite);
        check.Equal("blip position x reads back", 1f, q1.Position.X);
        check.Equal("blip position z reads back", 3f, q1.Position.Z);
        check.Equal("authoring rendered the blip", "Blip", fake.LastGlobalFunction);

        check.That("shared create broadcast sb:set for the blip bag",
            rec.Emits.Exists(e => e.Name == "sb:set" && e.Args[0].AsString() == "blip:q1"));
        check.That("broadcast went to everyone",
            rec.Emits.Exists(e => e.Name == "sb:set" && e.Target == RpcTarget.Broadcast &&
                                  e.Args[0].AsString() == "blip:q1"));
        check.That("a coordinate was broadcast",
            rec.Emits.Exists(e => e.Name == "sb:set" && e.Args[0].AsString() == "blip:q1" &&
                                  e.Args[1].AsString() == "x"));

        q1.Label = "Updated";
        check.Equal("changed label reads back", "Updated", Blips.Get("q1")!.Label);
        check.Equal("a field change re-renders the blip", "Blip", fake.LastGlobalFunction);

        Blips.Remove("q1");
        check.That("removed blip is gone", Blips.Get("q1") == null);
        check.Equal("registry is empty after remove", 0, Blips.All.Count);
        check.Equal("remove cleared the HUD blip", "ClearBlip", fake.LastGlobalFunction);

        Map.Reset();
        Platform.Reset();

        // --- client: a server-replicated shared blip renders locally ---
        Rpc.Clear();
        rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Client);
        Map.Bind(NetRole.Client);

        check.That("CreateShared is a no-op on a client",
            Blips.CreateShared("nope", Vector3.Zero, "x", BlipSprite.Default) == null);
        check.That("no bag was created by the no-op", StateBags.Find("blip:nope") == null);

        ServerSets("blip:s1", Blip.KeyX, Value.Float(10f));
        ServerSets("blip:s1", Blip.KeyY, Value.Float(20f));
        ServerSets("blip:s1", Blip.KeyZ, Value.Float(30f));
        ServerSets("blip:s1", Blip.KeyLabel, Value.String("Shop"));
        ServerSets("blip:s1", Blip.KeySprite, Value.Int((int)BlipSprite.Shop));

        check.That("client sees the replicated blip", Blips.Get("s1") != null);
        check.Equal("replicated blip is in the registry", 1, Blips.All.Count);
        Blip s1 = Blips.Get("s1")!;
        check.Equal("replicated label", "Shop", s1.Label);
        check.Equal("replicated sprite", BlipSprite.Shop, s1.Sprite);
        check.Equal("replicated position y", 20f, s1.Position.Y);
        check.Equal("replication rendered the blip", "Blip", fake.LastGlobalFunction);

        // --- local blip on a client never hits the wire ---
        rec.Emits.Clear();
        Blip local = Blips.CreateLocal("me", new Vector3(5, 6, 7), "Me", BlipSprite.Player);
        check.That("local blip is live", Blips.Get("me") != null);
        check.Equal("local create emits nothing to the server", 0, rec.Emits.Count);
        check.Equal("local blip reads back", "Me", Blips.Get("me")!.Label);
        check.Equal("local blip rendered", "Blip", fake.LastGlobalFunction);

        rec.Emits.Clear();
        Blips.Remove("me");
        check.That("local blip removed", Blips.Get("me") == null);
        check.Equal("local remove emits nothing", 0, rec.Emits.Count);

        // --- waypoint set / clear round-trip ---
        check.That("waypoint starts unset", Map.Waypoint == null);
        Map.SetWaypoint(new Vector3(100, 200, 300));
        check.That("waypoint is set", Map.Waypoint != null);
        check.Equal("waypoint x round-trips", 100f, Map.Waypoint!.Value.X);
        check.Equal("waypoint rendered", "Waypoint", fake.LastGlobalFunction);
        Map.ClearWaypoint();
        check.That("waypoint cleared", Map.Waypoint == null);
        check.Equal("waypoint clear rendered", "ClearWaypoint", fake.LastGlobalFunction);

        // --- reset clears everything ---
        Map.SetWaypoint(new Vector3(1, 1, 1));
        Blips.CreateLocal("a", Vector3.One, "A", BlipSprite.Marker);
        Map.Reset();
        check.That("reset clears blips", Blips.Get("a") == null);
        check.Equal("reset empties the registry", 0, Blips.All.Count);
        check.That("reset clears the waypoint", Map.Waypoint == null);

        Map.Reset();
        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
