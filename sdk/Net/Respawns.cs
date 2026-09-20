using System;
using Recreation.Modding;

namespace Recreation.Net;

// What dying means. The engine only marks a player dead -- it replicates the flag
// and stops them being a target -- because what happens next is the ruleset's
// call, and every game answers it differently. This is the answer a session gets
// when nobody has written a better one: wait, then put them back on their feet at
// the spawn with a full pool.
//
// A ruleset that wants its own handling sets Delay to a different number, or
// turns this off with Enabled = false and subscribes to PlayerVitalsChanged
// itself. Host-side only: a client cannot respawn anybody.
public static class Respawns
{
    // Seconds a player stays down before the default respawn. Under a second is
    // treated as the next tick.
    public static float Delay { get; set; } = 5f;

    // Whether the default respawn runs at all.
    public static bool Enabled { get; set; } = true;

    // Peers waiting on a respawn, with the seconds left. A player who is revived
    // some other way in the meantime drops out.
    private static readonly System.Collections.Generic.Dictionary<uint, float> Waiting = new();
    private static IDisposable? _vitals;
    private static IDisposable? _frame;

    public static void Bind(NetRole role)
    {
        Reset();
        if (role == NetRole.Client) return;  // only a host owns the bodies
        _vitals = EventBus.Subscribe<PlayerVitalsChanged>(OnVitals);
        _frame = EventBus.Subscribe<FrameUpdate>(e => Tick(e.DeltaTime));
    }

    public static void Reset()
    {
        _vitals?.Dispose();
        _vitals = null;
        _frame?.Dispose();
        _frame = null;
        Waiting.Clear();
        Delay = 5f;
        Enabled = true;
    }

    private static void OnVitals(PlayerVitalsChanged e)
    {
        // Peer 0 with no net id is not a player this host can act on.
        if (e.Peer == 0 && e.NetId == 0) return;
        if (e.Dead)
        {
            if (Enabled && !Waiting.ContainsKey(e.Peer)) Waiting[e.Peer] = Delay;
            return;
        }
        // Back up by some other means (a mod's SetHealth, an earlier respawn).
        Waiting.Remove(e.Peer);
    }

    private static void Tick(float deltaTime)
    {
        if (Waiting.Count == 0) return;
        uint[] peers = new uint[Waiting.Count];
        Waiting.Keys.CopyTo(peers, 0);
        foreach (uint peer in peers)
        {
            float left = Waiting[peer] - deltaTime;
            if (left > 0f)
            {
                Waiting[peer] = left;
                continue;
            }
            Waiting.Remove(peer);
            // Straight to the engine rather than through the roster: a player
            // whose presence has not replicated is still lying on the floor and
            // still deserves to get up. The engine restores the pool and clears
            // the flag, which replicates.
            Interop.Native.CallGlobal("Net", "RespawnPlayer",
                                      new[] { Interop.Value.Int((int)peer) });
        }
    }
}
