using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when the player crosses into or out of being over-encumbered.
public readonly struct EncumbranceChanged(bool overEncumbered, float carried, float capacity)
    : IGameEvent
{
    public bool OverEncumbered { get; } = overEncumbered;
    public float CarriedWeight { get; } = carried;
    public float Capacity { get; } = capacity;
}

// Skyrim's carry-weight rule, implemented as managed soft logic: when the weight
// the player carries exceeds their CarryWeight, they cannot fast travel and move
// slower until they drop something. It sums the inventory (each item's weight
// times its count) against the carry-weight actor value, applying and removing a
// speed penalty exactly once per transition so it never stacks or leaks.
//
// The sum walks the inventory, so it recomputes on an interval rather than every
// frame. SpeedMult is adjusted relatively (ModValue), composing with other speed
// effects such as the injury limp.
public sealed class Encumbrance : GameBehaviour
{
    // Seconds between inventory weight recomputations.
    public float RecomputeInterval { get; set; } = 1.0f;
    // Points removed from SpeedMult while over-encumbered.
    public float SpeedPenalty { get; set; } = 40f;

    private float _timer;
    private bool _overEncumbered;

    protected override void OnUpdate(float deltaTime)
    {
        _timer += deltaTime;
        if (_timer < RecomputeInterval) return;
        _timer = 0f;

        Actor player = Game.Player;
        if (!player.Exists) return;

        float carried = TotalWeight(player);
        float capacity = player.GetValue(ActorValue.CarryWeight);
        bool over = carried > capacity;
        if (over == _overEncumbered) return;

        _overEncumbered = over;
        Game.EnableFastTravel(!over);
        player.ModValue(ActorValue.SpeedMult, over ? -SpeedPenalty : SpeedPenalty);
        EventBus.Publish(new EncumbranceChanged(over, carried, capacity));
    }

    // Total weight a container carries: each distinct item's unit weight times the
    // count held.
    public static float TotalWeight(ObjectReference holder)
    {
        float total = 0f;
        foreach (Form item in holder.Items())
            total += item.Weight * holder.GetItemCount(item);
        return total;
    }
}
