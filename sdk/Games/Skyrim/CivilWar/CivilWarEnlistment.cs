using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// A modern "choose your side" prompt for the Civil War: F1 enlists with the
// Imperial Legion, F2 with the Stormcloaks. Each starts that side's enlistment
// quest and commits its stage, which CivilWarAllegianceTracker reads back into
// CivilWarState, so the war map, ranks and sieges all follow the player's
// choice. It is purely additive over the OG dialogue enlistment: it offers
// nothing once a side is settled (the war has no defection), and joining by any
// path raises AllegianceChanged, which retires the hotkeys. The engine reports
// the key press; the gameplay decides what it means.
public sealed class CivilWarEnlistment : GameBehaviour
{
    private EventBus.Subscription? _imperialKey;
    private EventBus.Subscription? _stormcloakKey;

    protected override void OnStart()
    {
        if (CivilWarState.HasJoined) return;  // already committed; nothing to offer
        _imperialKey = Hotkeys.Bind(Key.F1, () => Enlist(CivilWarSide.Imperial));
        _stormcloakKey = Hotkeys.Bind(Key.F2, () => Enlist(CivilWarSide.Stormcloak));
        CivilWarState.AllegianceChanged += OnAllegianceSettled;
        Debug.Notification("Civil War: press F1 to join the Imperial Legion, F2 for the Stormcloaks.");
    }

    protected override void OnDestroy()
    {
        _imperialKey?.Dispose();
        _stormcloakKey?.Dispose();
        CivilWarState.AllegianceChanged -= OnAllegianceSettled;
    }

    private static void Enlist(CivilWarSide side)
    {
        if (CivilWarState.HasJoined) return;
        uint questId = side == CivilWarSide.Imperial ? CivilWarForms.JoinImperialQuest
                                                     : CivilWarForms.JoinStormcloakQuest;
        Quest join = Game.GetQuest(questId);
        if (!join.Exists) return;
        if (!join.IsRunning) join.Start();
        // Any stage past the set-up stage commits the side; the allegiance tracker
        // reads which one from the quest's form id.
        join.Stage = 10;
    }

    // The tracker (or the OG dialogue) settled the side: retire the hotkeys and
    // confirm, so we never offer a choice the player no longer has.
    private void OnAllegianceSettled(CivilWarSide side)
    {
        _imperialKey?.Dispose();
        _imperialKey = null;
        _stormcloakKey?.Dispose();
        _stormcloakKey = null;
        Debug.Notification(side == CivilWarSide.Imperial ? "You have enlisted with the Imperial Legion!"
                                                         : "You have enlisted with the Stormcloaks!");
    }
}
