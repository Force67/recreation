using System.Linq;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised when the player crosses into or out of being over-mass.
public readonly struct OverMassChanged(bool overMass, float carried, float capacity) : IGameEvent
{
    public bool OverMass { get; } = overMass;
    public float CarriedMass { get; } = carried;
    public float Capacity { get; } = capacity;
}

// Starfield's mass limit as managed soft logic: when carried mass exceeds the
// player's capacity they cannot fast travel (grav-jump) and move slower, and
// hauling the excess burns oxygen faster by feeding the O2/CO2 loop. Mirrors
// Skyrim's Encumbrance over the same TotalWeight/CarryWeight values but adds the
// exertion tie-in unique to Starfield's survival model. The penalty and gate are
// applied once per transition so they never stack or leak.
//
// The mass sum walks the inventory, so it recomputes on an interval rather than
// every frame, and immediately on an inventory change so picking up loot reacts at
// once. SpeedMult is adjusted relatively (ModValue), composing with other speed
// effects.
public sealed class MassEncumbrance : GameBehaviour
{
    // Seconds between mass recomputations.
    public float RecomputeInterval { get; set; } = 1.0f;
    // Points removed from SpeedMult while over-mass.
    public float SpeedPenalty { get; set; } = 35f;

    private float _timer;
    private bool _overMass;
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
        if (over == _overMass) return;

        _overMass = over;
        if (over) FastTravel.Block("mass");
        else FastTravel.Unblock("mass");
        player.ModValue(ActorValue.SpeedMult, over ? -SpeedPenalty : SpeedPenalty);

        // Over-mass counts as exertion, so the O2 loop drains while hauling excess.
        foreach (OxygenCo2 oxygen in ModHost.ActiveBehaviours.OfType<OxygenCo2>())
            oxygen.Overburdened = over;

        EventBus.Publish(new OverMassChanged(over, carried, capacity));
    }
}
