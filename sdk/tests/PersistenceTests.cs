using System;
using System.IO;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the durable storage subsystem: in-memory and namespaced stores, JSON file
// round-trip and crash/missing-file tolerance, and per-player save/load with leave auto-save.
public static class PersistenceTests
{
    public static void Run(Check check)
    {
        MemoryStore(check);
        Namespaced(check);
        JsonFile(check);
        PlayerSaveLoad(check);
    }

    private static void MemoryStore(Check check)
    {
        var store = new MemoryKvStore();
        store.Set("hp", Value.Int(42));
        store.Set("speed", Value.Float(1.5f));
        store.Set("alive", Value.Bool(true));
        store.Set("name", Value.String("Dragonborn"));

        check.Equal("memory int round-trips", 42, store.Get("hp").AsInt());
        check.Equal("memory float round-trips", 1.5f, store.Get("speed").AsFloat());
        check.That("memory bool round-trips", store.Get("alive").AsBool());
        check.Equal("memory string round-trips", "Dragonborn", store.Get("name").AsString());
        check.That("memory has set key", store.Has("hp"));
        check.That("memory lacks unset key", !store.Has("missing"));
        check.That("memory get of unset is None", store.Get("missing").IsNone);
        check.Equal("memory reports every key", 4, store.Keys.Count);

        store.Delete("hp");
        check.That("memory delete removes the key", !store.Has("hp"));
        check.Equal("memory key count drops after delete", 3, store.Keys.Count);

        store.Set("alive", Value.None);
        check.That("setting None deletes", !store.Has("alive"));
    }

    private static void Namespaced(Check check)
    {
        var backing = new MemoryKvStore();
        IKvStore a = backing.Scope("a");
        IKvStore b = backing.Scope("b");

        a.Set("gold", Value.Int(10));
        b.Set("gold", Value.Int(20));

        check.Equal("scope a keeps its own value", 10, a.Get("gold").AsInt());
        check.Equal("scope b keeps its own value", 20, b.Get("gold").AsInt());
        check.That("scope a has only its key", a.Has("gold"));
        check.Equal("scope a lists one key", 1, a.Keys.Count);
        check.Equal("both scopes share one backing store", 2, backing.Keys.Count);

        a.Delete("gold");
        check.That("deleting in scope a is local to a", !a.Has("gold"));
        check.That("scope b is untouched by a's delete", b.Has("gold"));
    }

    private static void JsonFile(Check check)
    {
        string dir = Path.Combine(Path.GetTempPath(), "rec_persist_test_" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(dir, "save.json");
        try
        {
            var disk = new JsonFileKvStore(path);
            disk.Set("level", Value.Int(7));
            disk.Set("mass", Value.Float(2.25f));
            disk.Set("hardcore", Value.Bool(true));
            disk.Set("title", Value.String("Thane"));
            disk.Set("actor", Value.Object(0xDEADBEEFUL));
            disk.Flush();

            // A brand new store over the same path must reconstruct every value with
            // the right kind and payload.
            var reloaded = new JsonFileKvStore(path);
            check.Equal("json int reloads as Int", ValueKind.Int, reloaded.Get("level").Kind);
            check.Equal("json int payload", 7, reloaded.Get("level").AsInt());
            check.Equal("json float reloads as Float", ValueKind.Float, reloaded.Get("mass").Kind);
            check.Equal("json float payload", 2.25f, reloaded.Get("mass").AsFloat());
            check.Equal("json bool reloads as Bool", ValueKind.Bool, reloaded.Get("hardcore").Kind);
            check.That("json bool payload", reloaded.Get("hardcore").AsBool());
            check.Equal("json string reloads as String", ValueKind.String, reloaded.Get("title").Kind);
            check.Equal("json string payload", "Thane", reloaded.Get("title").AsString());
            check.Equal("json object reloads as Object", ValueKind.Object, reloaded.Get("actor").Kind);
            check.Equal("json object handle payload", 0xDEADBEEFUL, reloaded.Get("actor").AsHandle());

            // A path that does not exist must start empty and not throw; writing then
            // works and persists (including creating missing parent directories).
            string missing = Path.Combine(dir, "nested", "deeper", "fresh.json");
            var fresh = new JsonFileKvStore(missing);
            check.Equal("missing file starts empty", 0, fresh.Keys.Count);
            fresh.Set("ok", Value.Int(1));
            check.That("write after a missing file works", fresh.Has("ok"));
            var freshReload = new JsonFileKvStore(missing);
            check.Equal("missing-file write was persisted", 1, freshReload.Get("ok").AsInt());

            // A corrupt file is tolerated: start empty, no throw, writing recovers.
            string corruptPath = Path.Combine(dir, "corrupt.json");
            File.WriteAllText(corruptPath, "{ this is not valid json ]");
            var corrupt = new JsonFileKvStore(corruptPath);
            check.Equal("corrupt file starts empty", 0, corrupt.Keys.Count);
            corrupt.Set("recovered", Value.Bool(true));
            check.That("write after a corrupt file works", corrupt.Get("recovered").AsBool());
        }
        finally
        {
            try { if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true); }
            catch { /* best-effort temp cleanup */ }
        }
    }

    private static void PlayerSaveLoad(Check check)
    {
        Platform.Boot(NetRole.Server);
        PlayerData.Reset();  // start from a known-clean configuration

        // Register a peer so it enters the roster, then give it a name and some bag state.
        EventBus.Publish(new ClientJoined(3u));
        Net.Player? player = Players.Get(3);
        check.That("joined peer is in the roster", player != null);

        StateBags.Player(3).Set("name", "Lydia");
        StateBags.Player(3).Set("gold", 100);
        StateBags.Player(3).Set("level", 5);

        var store = new MemoryKvStore();
        PlayerData.UseStore(store);
        PlayerData.Persist("gold", "level");

        string id = PlayerData.IdentityOf(player!);
        check.Equal("identity uses the player name", "Lydia", id);

        // Save copies only the persisted keys into the player's slot.
        PlayerData.Save(player);
        IKvStore slot = store.Scope(id);
        check.Equal("save wrote gold", 100, slot.Get("gold").AsInt());
        check.Equal("save wrote level", 5, slot.Get("level").AsInt());
        check.That("save ignored the un-persisted name", !slot.Has("name"));

        // Wipe the live values, then Load must restore them from the store.
        StateBags.Player(3).Remove("gold");
        StateBags.Player(3).Remove("level");
        check.That("bag value cleared before load", !StateBags.Player(3).Has("gold"));

        PlayerData.Load(player);
        check.Equal("load restored gold", 100, StateBags.Player(3).Get("gold").AsInt());
        check.Equal("load restored level", 5, StateBags.Player(3).Get("level").AsInt());

        // After Bind, a leaving player auto-saves. Change a value first so the assert
        // proves the auto-save ran (captured the latest state), not a stale write.
        PlayerData.Bind(NetRole.Server);
        StateBags.Player(3).Set("gold", 250);
        EventBus.Publish(new ClientLeft(3u));
        check.That("client-left auto-saved the slot", slot.Has("gold"));
        check.Equal("auto-save captured the latest gold", 250, slot.Get("gold").AsInt());

        PlayerData.Reset();
        Platform.Reset();
        EventBus.Clear();
    }
}
