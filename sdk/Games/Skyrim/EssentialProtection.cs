using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Keeps essential actors from dying, Skyrim's rule that story-critical NPCs go
// down and get back up rather than being killed. When an actor's health reaches
// zero, this checks its base for the essential flag and, if set, restores a
// slice of health so it recovers. Event-driven soft logic; ordinary actors are
// untouched.
public sealed class EssentialProtection : GameBehaviour
{
    private EventBus.Subscription? _subscription;

    // Health restored on revival, as a fraction of the actor's base health.
    public float ReviveFraction { get; set; } = 0.25f;

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<ActorDied>(OnActorDied);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnActorDied(ActorDied e)
    {
        Actor actor = e.Actor;
        if (!actor.IsEssential) return;
        float baseHealth = actor.GetBaseValue(ActorValue.Health);
        if (baseHealth <= 0f) return;
        actor.RestoreValue(ActorValue.Health, baseHealth * ReviveFraction);
    }
}
