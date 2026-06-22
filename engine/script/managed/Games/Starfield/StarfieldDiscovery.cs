using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised the first time the player reaches a given location.
public readonly struct LocationDiscovered(ulong cellHandle) : IGameEvent
{
    public ulong CellHandle { get; } = cellHandle;
}

// Exploration discovery and its reward: the first time the player reaches a place
// the engine names (the interiors its load-door transitions report), it is
// remembered, announced, and pays character experience, the way Starfield rewards
// finding new locations. Drives off the engine's LocationChanged, so it is a third
// live source of XP beside quests and combat. Feeds CharacterProgress. Persistent
// player state, so it outlives the mod-host lifecycle; tests clear it via Clear.
public sealed class StarfieldDiscovery : GameBehaviour
{
    // Character XP awarded the first time a location is reached.
    public float XpPerDiscovery { get; set; } = 25f;

    private readonly HashSet<ulong> _visited = new();
    private EventBus.Subscription? _subscription;

    public int DiscoveredCount => _visited.Count;
    public bool HasDiscovered(ulong cellHandle) => _visited.Contains(cellHandle);

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<LocationChanged>(OnLocationChanged);

    protected override void OnDestroy() => _subscription?.Dispose();

    // Resets the discovery log, for a new game or a test.
    public void Clear() => _visited.Clear();

    private void OnLocationChanged(LocationChanged e)
    {
        // The engine reports cell 0 when stepping back outside; only named
        // locations (interiors today) count, and only their first visit.
        if (e.CellHandle == 0 || !_visited.Add(e.CellHandle)) return;
        CharacterProgress.GainXp(Game.Player, XpPerDiscovery);
        EventBus.Publish(new LocationDiscovered(e.CellHandle));
    }
}
