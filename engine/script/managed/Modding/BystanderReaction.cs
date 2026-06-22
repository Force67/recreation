using Recreation;

namespace Recreation.Modding;

// Why a bystander is alarmed: what they witnessed.
public enum AlarmReason
{
    Death,   // someone died nearby: flee or raise the hue and cry
    Theft,   // an item was taken near its owner: call it out
}

// Raised when an NPC reacts to a crime or a death it witnessed nearby. One is
// fired per witness; the AI turns it into a flee, a shout or a guard alert. Pure
// reaction state, raised by proximity, so a future C++ AI reads "this NPC saw a
// killing 300 units away" without this moving anyone.
public readonly struct BystanderAlarmed(ulong witnessHandle, AlarmReason reason, ulong subjectHandle)
    : IGameEvent
{
    public ulong WitnessHandle { get; } = witnessHandle;
    public AlarmReason Reason { get; } = reason;

    // What was witnessed: the actor who died, or the item that was taken.
    public ulong SubjectHandle { get; } = subjectHandle;

    public Actor Witness => Actor.From(WitnessHandle);
}

// The bystander layer: NPCs react to alarming events that happen near them. When an
// actor dies, every living actor within earshot is alarmed (Death); when an item
// leaves a container near another actor, that owner is alarmed (Theft). It reads
// the engine's generic ActorDied and ItemRemoved events and the proximity snapshot,
// raises a BystanderAlarmed per witness, and stops there: the reaction state is the
// product, not a movement.
//
// Driven by events plus proximity: it does nothing until the engine raises a death
// or an item removal, so it has no per-frame cost.
public sealed class BystanderReaction : GameBehaviour
{
    // How far (game units) a death or a theft is noticed. The defaults match a
    // scream carrying across a room and a theft seen at arm's length.
    public float DeathAlarmRadius { get; set; } = 800f;
    public float TheftAlarmRadius { get; set; } = 300f;

    private EventBus.Subscription? _deathSub;
    private EventBus.Subscription? _theftSub;

    protected override void OnStart()
    {
        _deathSub = EventBus.Subscribe<ActorDied>(OnActorDied);
        _theftSub = EventBus.Subscribe<ItemRemoved>(OnItemRemoved);
    }

    protected override void OnDestroy()
    {
        _deathSub?.Dispose();
        _theftSub?.Dispose();
        _deathSub = null;
        _theftSub = null;
    }

    // A death alarms every living actor within earshot of the body.
    private void OnActorDied(ActorDied e)
    {
        Actor dead = e.Actor;
        foreach (NearbyRef near in dead.RefsNear(DeathAlarmRadius))
        {
            var witness = Actor.From(near.Reference.Handle);
            if (witness.IsDead) continue;
            EventBus.Publish(new BystanderAlarmed(witness.Handle, AlarmReason.Death, dead.Handle));
        }
    }

    // An item leaving a container near a living actor reads as a theft that actor
    // witnessed. The container itself is skipped (a chest does not cry out).
    private void OnItemRemoved(ItemRemoved e)
    {
        ObjectReference container = e.Container;
        foreach (NearbyRef near in container.RefsNear(TheftAlarmRadius))
        {
            if (near.Reference.Handle == container.Handle) continue;
            var witness = Actor.From(near.Reference.Handle);
            if (witness.IsDead) continue;
            EventBus.Publish(new BystanderAlarmed(witness.Handle, AlarmReason.Theft, e.Item.Handle));
        }
    }
}
