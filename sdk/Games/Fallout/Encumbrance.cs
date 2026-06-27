using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Raised when the player crosses into or out of being over-encumbered.
public readonly struct EncumbranceChanged(bool overEncumbered, float carried, float capacity)
    : IGameEvent
{
    public bool OverEncumbered { get; } = overEncumbered;
    public float CarriedWeight { get; } = carried;
    public float Capacity { get; } = capacity;
}

// Fallout 4's carry-weight rule as managed soft logic: when the weight the player
// carries exceeds their CarryWeight (which Strength sets, see Special) they cannot
// fast travel and move slower until they drop something. It sums the inventory
// against the carry-weight actor value, applying and removing a SpeedMult penalty
// and the fast-travel block exactly once per transition so they never stack or leak.
// The Fallout twin of Skyrim's Encumbrance and Starfield's MassEncumbrance.
//
// The sum walks the inventory, so it recomputes on an interval rather than every
// frame, and immediately on an inventory change so picking up loot reacts at once.
public sealed class Encumbrance : GameBehaviour
{
    // Seconds between inventory weight recomputations.
    public float RecomputeInterval { get; set; } = 1.0f;
    // Points removed from SpeedMult while over-encumbered.
    public float SpeedPenalty { get; set; } = 40f;

    private float _timer;
    private bool _overEncumbered;
    private bool _dirty = true;  // force a recompute on the first frame
    private EventBus.Subscription? _added;
    private EventBus.Subscription? _removed;

    protected override void OnStart()
    {
        _added = EventBus.Subscribe<ItemAdded>(e => MarkDirtyIfPlayer(e.ContainerHandle));
        _removed = EventBus.Subscribe<ItemRemoved>(e => MarkDirtyIfPlayer(e.ContainerHandle));
    }

    protected override void OnDestroy()
    {
        _added?.Dispose();
        _removed?.Dispose();
    }

    private void MarkDirtyIfPlayer(ulong container)
    {
        if (container == Game.Player.Handle) _dirty = true;
    }

    protected override void OnUpdate(float deltaTime)
    {
        _timer += deltaTime;
        if (!_dirty && _timer < RecomputeInterval) return;
        _timer = 0f;
        _dirty = false;

        Actor player = Game.Player;
        if (!player.Exists) return;

        float carried = player.TotalWeight;
        float capacity = player.GetValue(ActorValue.CarryWeight);
        bool over = carried > capacity;
        if (over == _overEncumbered) return;

        _overEncumbered = over;
        if (over) FastTravel.Block("encumbrance");
        else FastTravel.Unblock("encumbrance");
        player.ModValue(ActorValue.SpeedMult, over ? -SpeedPenalty : SpeedPenalty);
        EventBus.Publish(new EncumbranceChanged(over, carried, capacity));
    }
}
