using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised the first time the player enters a given location.
public readonly struct LocationDiscovered(ulong cellHandle) : IGameEvent
{
    public ulong CellHandle { get; } = cellHandle;

    public Cell Cell => Cell.From(CellHandle);
}

// Tracks which locations the player has visited and announces each one the first
// time, the basis for Skyrim's map-marker discovery, exploration XP and journal
// notes. It listens for LocationChanged and remembers the cells it has seen,
// publishing LocationDiscovered once per new location. It covers the interiors
// reached through load doors (the transitions the engine reports today).
public sealed class LocationDiscovery : GameBehaviour
{
    private readonly HashSet<ulong> _visited = new();
    private EventBus.Subscription? _subscription;

    public int VisitedCount => _visited.Count;
    public bool HasVisited(Cell cell) => _visited.Contains(cell.Handle);

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<LocationChanged>(OnLocationChanged);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnLocationChanged(LocationChanged e)
    {
        if (!e.IsInterior || e.CellHandle == 0) return;
        if (_visited.Add(e.CellHandle))
            EventBus.Publish(new LocationDiscovered(e.CellHandle));
    }
}
