using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Interop;

namespace Recreation.Net;

// The blip registry and authority. Creates blips, hands out handles, and keeps the
// rendered set on every machine in step with blip state. A SHARED blip is server-
// authored (replicates), a LOCAL blip is non-replicated. Rendering is driven by
// observing blip state-bag changes.
public static class Blips
{
    // Ids with a live (non-empty) bag on this machine, maintained by the observer.
    private static readonly HashSet<string> Live = new();
    // Ids created locally on this machine; their writes and removals stay local.
    private static readonly HashSet<string> Local = new();
    private static readonly List<IDisposable> Subscriptions = new();
    private static NetRole _role = NetRole.Standalone;

    // Wire rendering for a role: observe every blip bag so a change re-renders and
    // an emptied bag clears. Idempotent; called by Map.Bind.
    internal static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
    }

    internal static void Reset()
    {
        foreach (string id in new List<string>(Live)) ClearRender(id);
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        Live.Clear();
        Local.Clear();
        _role = NetRole.Standalone;
    }

    // A shared blip every player sees. Only the authority writes it; a no-op on a client.
    public static Blip? CreateShared(string id, Vector3 pos, string label, BlipSprite sprite,
                                     uint color = 0, bool shortRange = false)
    {
        if (_role == NetRole.Client)
        {
            Console.Error.WriteLine($"[map] CreateShared('{id}') ignored on a client; shared blips are server-authored");
            return null;
        }
        return Write(id, pos, label, sprite, color, shortRange, replicated: true);
    }

    // A blip only this machine sees. Written non-replicated.
    public static Blip CreateLocal(string id, Vector3 pos, string label, BlipSprite sprite,
                                   uint color = 0, bool shortRange = false)
    {
        Local.Add(id);
        return Write(id, pos, label, sprite, color, shortRange, replicated: false);
    }

    // The blip if it is live on this machine, else null.
    public static Blip? Get(string id) => Live.Contains(id) ? new Blip(id, !Local.Contains(id)) : null;

    // Every live blip (a fresh snapshot, safe to enumerate while it mutates).
    public static IReadOnlyCollection<Blip> All
    {
        get
        {
            var list = new List<Blip>(Live.Count);
            foreach (string id in Live) list.Add(new Blip(id, !Local.Contains(id)));
            return list;
        }
    }

    // Remove a blip by emptying its bag; the observer then clears the render.
    public static void Remove(string id)
    {
        StateBag? bag = StateBags.Find(Blip.BagNameFor(id));
        if (bag == null) return;
        bool replicated = !Local.Contains(id);
        // Snapshot the keys: Remove mutates the bag as we go.
        foreach (string key in new List<string>(bag.Keys)) bag.Remove(key, replicated);
    }

    // Write every field of a blip into its bag.
    private static Blip Write(string id, Vector3 pos, string label, BlipSprite sprite,
                             uint color, bool shortRange, bool replicated)
    {
        StateBag bag = StateBags.Bag(Blip.BagNameFor(id));
        bag.Set(Blip.KeyX, pos.X, replicated);
        bag.Set(Blip.KeyY, pos.Y, replicated);
        bag.Set(Blip.KeyZ, pos.Z, replicated);
        bag.Set(Blip.KeyLabel, label, replicated);
        bag.Set(Blip.KeySprite, (int)sprite, replicated);
        bag.Set(Blip.KeyColor, Value.Int(unchecked((int)color)), replicated);
        bag.Set(Blip.KeyShort, shortRange, replicated);
        return new Blip(id, replicated);
    }

    // Keep the rendered set in step with replicated state. Runs on every machine for
    // every bag change; ignores non-blip bags.
    private static void OnBagChange(StateBagChange change)
    {
        string name = change.Bag.Name;
        if (!name.StartsWith(Blip.BagPrefix, StringComparison.Ordinal)) return;
        string id = name.Substring(Blip.BagPrefix.Length);

        // An emptied bag means the blip was removed (the last field is gone). The
        // entries are already mutated when the observer fires, so Count is current.
        if (change.Bag.Count == 0)
        {
            if (Live.Remove(id))
            {
                Local.Remove(id);
                ClearRender(id);
            }
            return;
        }

        Live.Add(id);
        Render(id, change.Bag);
    }

    private static void Render(string id, StateBag bag) =>
        CallHud("Blip",
            Value.String(id),
            Value.Float(bag.Get(Blip.KeyX).AsFloat()),
            Value.Float(bag.Get(Blip.KeyY).AsFloat()),
            Value.Float(bag.Get(Blip.KeyZ).AsFloat()),
            Value.String(bag.Get(Blip.KeyLabel).AsString()),
            Value.Int(bag.Get(Blip.KeySprite).AsInt()),
            Value.Int(bag.Get(Blip.KeyColor).AsInt()),
            Value.Bool(bag.Get(Blip.KeyShort).AsBool()));

    private static void ClearRender(string id) => CallHud("ClearBlip", Value.String(id));

    // Route to the engine HUD. Returns None when the HUD is not wired, so this is
    // safe to call on a headless server too.
    private static void CallHud(string function, params ReadOnlySpan<Value> args) =>
        Native.CallGlobal("Hud", function, args);
}
