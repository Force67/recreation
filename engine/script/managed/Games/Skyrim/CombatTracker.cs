using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Keeps simple combat statistics by listening for actor deaths, the foundation
// other mods (achievements, bounties, UI) build on. The player's own death is
// not counted as a kill. Event-driven: nothing here polls the world.
public sealed class CombatTracker : GameBehaviour
{
    private EventBus.Subscription? _subscription;

    // Actors slain since the world came up (excluding the player).
    public int Kills { get; private set; }

    // The most recently slain actor, or Actor with handle 0 before any kill.
    public Actor LastVictim { get; private set; } = Actor.From(0);

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<ActorDied>(OnActorDied);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnActorDied(ActorDied e)
    {
        if (e.ActorHandle == Game.Player.Handle) return;  // the player dying is not a kill
        Kills++;
        LastVictim = e.Actor;
    }
}
