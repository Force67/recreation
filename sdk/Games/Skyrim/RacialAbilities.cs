using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Grants the player their race's passive abilities once they are loaded: the
// engine does not apply racial abilities on its own, so this is the system that
// makes a Nord resist frost and an Argonian breathe water. It polls until the
// player and their race resolve, grants once, then idles. Player-only (the common
// case and cheap); a mod grants an NPC with RaceTraits.Grant directly.
public sealed class RacialAbilities : GameBehaviour
{
    private bool _granted;

    protected override void OnUpdate(float deltaTime)
    {
        if (_granted) return;

        Actor player = Game.Player;
        if (!player.Exists) return;            // the player is not loaded yet
        if (player.Race.Abilities.Count == 0)  // the race has not resolved yet
            return;

        RaceTraits.Grant(player);
        _granted = true;
    }
}
