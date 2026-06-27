using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Grants the player character experience for kills made during their own fights,
// the other half of the Starfield XP loop alongside quest rewards. It listens for
// actor deaths and pays out only when the player is in combat at the moment of the
// kill, the same simplification Skyrim's CombatTracker settles for (the engine
// raises no killer attribution), so XP follows the player's encounters rather than
// every distant death. Feeds CharacterProgress.
public sealed class StarfieldCombatRewards : GameBehaviour
{
    // Character XP for a kill made while the player is in combat.
    public float XpPerKill { get; set; } = 15f;

    private EventBus.Subscription? _sub;

    protected override void OnStart() => _sub = EventBus.Subscribe<ActorDied>(OnDeath);

    protected override void OnDestroy() => _sub?.Dispose();

    private void OnDeath(ActorDied e)
    {
        Actor player = Game.Player;
        if (e.ActorHandle == player.Handle) return;  // the player's own death pays nothing
        if (!player.IsInCombat) return;              // only kills during the player's fights
        CharacterProgress.GainXp(player, XpPerKill);
    }
}
