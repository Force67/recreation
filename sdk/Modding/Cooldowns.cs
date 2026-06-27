using System.Collections.Generic;

namespace Recreation.Modding;

// Tracks named cooldowns, the timing a mod needs to gate powers, shouts and
// abilities: start a cooldown under a key, then ask whether it is ready before
// firing again. Real-time and driven by the mod host each frame, entirely in
// managed code.
//
//   if (Cooldowns.IsReady("fireball")) {
//       CastFireball();
//       Cooldowns.Start("fireball", 3f);
//   }
public static class Cooldowns
{
    private static readonly Dictionary<string, double> ReadyAt = new();
    private static double _clock;

    // Starts (or restarts) a cooldown of `seconds` under `key`.
    public static void Start(string key, float seconds) => ReadyAt[key] = _clock + seconds;

    // True when no cooldown under `key` is active.
    public static bool IsReady(string key) =>
        !ReadyAt.TryGetValue(key, out double ready) || _clock >= ready;

    // Seconds left on `key`'s cooldown, 0 if ready.
    public static float Remaining(string key) =>
        ReadyAt.TryGetValue(key, out double ready) && ready > _clock
            ? (float)(ready - _clock)
            : 0f;

    // Clears a single cooldown, making it ready immediately.
    public static void Reset(string key) => ReadyAt.Remove(key);

    public static void Clear()
    {
        ReadyAt.Clear();
        _clock = 0;
    }

    // Advances the cooldown clock; called by the mod host each frame.
    public static void Advance(float deltaTime) => _clock += deltaTime;
}
