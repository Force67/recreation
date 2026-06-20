using System.Collections.Generic;

namespace Recreation.Modding;

// Drives every active Zone from the frame loop, throttled so the proximity query
// is not rerun every frame. Mods rarely touch this directly: Zone.Around registers
// a zone here, and disposing it unregisters. The first zone subscribes to the
// frame event; the managed-world teardown clears them all.
public static class Zones
{
    private static readonly List<Zone> Active = new();
    private static EventBus.Subscription? _tick;
    private static float _sinceScan;

    // Seconds between membership scans. A coarse default keeps many zones cheap;
    // set to 0 to scan every frame.
    public static float ScanInterval { get; set; } = 0.25f;

    public static int ActiveCount => Active.Count;

    internal static void Register(Zone zone)
    {
        Active.Add(zone);
        _tick ??= EventBus.Subscribe<FrameUpdate>(OnFrame);
    }

    internal static void Unregister(Zone zone) => Active.Remove(zone);

    private static void OnFrame(FrameUpdate frame)
    {
        _sinceScan += frame.DeltaTime;
        if (_sinceScan < ScanInterval) return;
        _sinceScan = 0f;
        // Snapshot so a zone's callback may create or dispose zones mid-scan.
        foreach (Zone zone in Active.ToArray()) zone.Refresh();
    }

    // Drops all zones and the frame subscription. Used on managed-world teardown.
    public static void Clear()
    {
        Active.Clear();
        _sinceScan = 0f;
        _tick?.Dispose();
        _tick = null;
    }
}
