using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Enforces Skyrim's rule that the player cannot fast travel while in combat,
// implemented as managed soft logic. It watches the player's combat state and
// only toggles fast travel when that state changes, so it costs one query a
// frame and an engine call only on the transition in or out of combat.
public sealed class CombatFastTravelLock : GameBehaviour
{
    private bool? _lastInCombat;

    protected override void OnUpdate(float deltaTime)
    {
        Actor player = Game.Player;
        if (!player.Exists) return;

        bool inCombat = player.IsInCombat;
        if (_lastInCombat == inCombat) return;
        _lastInCombat = inCombat;
        if (inCombat) FastTravel.Block("combat");
        else FastTravel.Unblock("combat");
    }
}
