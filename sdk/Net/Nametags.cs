using System;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Floating player nametags. Each frame this pushes a world-space label for every
// other player that has a known position (px,py,pz in its state bag); the engine
// projects and draws it. Also publishes the local player's position to its own bag
// a few times a second so others can tag it. You never tag yourself.
public static class Nametags
{
    // Reserved position keys in a player's state bag (engine-space world position).
    internal const string PosX = "px";
    internal const string PosY = "py";
    internal const string PosZ = "pz";

    private static IDisposable? _tick;
    private static float _publishTimer;

    public static bool Enabled { get; set; } = true;

    // Metres the tag floats above the player's reported position.
    public static float HeightOffset { get; set; } = 2.0f;

    internal static void Bind(NetRole role)
    {
        Reset();
        _tick = EventBus.Subscribe<FrameUpdate>(e => Tick(e.DeltaTime));
    }

    internal static void Reset()
    {
        _tick?.Dispose();
        _tick = null;
        _publishTimer = 0;
    }

    private static void Tick(float dt)
    {
        if (!Enabled) return;
        PublishLocalPosition(dt);
        foreach (Player p in Players.All)
        {
            if (p.IsLocal || !p.State.Has(PosX)) continue;
            Native.CallGlobal("Hud", "Nametag", new[]
            {
                Value.String(p.Name),
                Value.Float(p.State.Get(PosX).AsFloat()),
                Value.Float(p.State.Get(PosY).AsFloat() + HeightOffset),
                Value.Float(p.State.Get(PosZ).AsFloat()),
                Value.Int(0),
            });
        }
    }

    // Push the local player's world position to its own bag at ~10 Hz. An own-bag
    // write, allowed on a client.
    private static void PublishLocalPosition(float dt)
    {
        if (!Platform.IsNetworked) return;
        _publishTimer += dt;
        if (_publishTimer < 0.1f) return;
        _publishTimer = 0;
        Vector3 p = Players.LocalWorldPos;
        StateBag bag = Players.Local.State;
        bag.Set(PosX, p.X);
        bag.Set(PosY, p.Y);
        bag.Set(PosZ, p.Z);
    }
}
