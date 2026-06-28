using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Promotes the player through the Civil War ranks as their side's campaign advances
// and toasts each promotion. It listens for hold captures (the campaign board's one
// source of war progress) and, when the player's own side takes a hold, credits a
// completed mission to CivilWarRank; the resulting rank-up surfaces as a "You have
// been promoted to Legate!" notification. Promotion is driven by campaign events
// only, never a timer, and is null-safe (no player or live quest is touched).
public sealed class CivilWarRankTracker : GameBehaviour
{
    protected override void OnStart()
    {
        CivilWarCampaign.HoldCaptured += OnHoldCaptured;
        CivilWarRank.RankChanged += OnRankChanged;
    }

    protected override void OnDestroy()
    {
        CivilWarCampaign.HoldCaptured -= OnHoldCaptured;
        CivilWarRank.RankChanged -= OnRankChanged;
        CivilWarRank.Reset();
    }

    // A hold fell: if the player's own side took it, that is a mission completed,
    // which may earn a promotion. Captures for the other side (or neutral) leave the
    // player's rank untouched.
    private void OnHoldCaptured(Hold hold, Side captor)
    {
        if (!IsPlayersSide(captor)) return;
        CivilWarRank.Promote();  // idempotent at the top rank, raises RankChanged on a real rank-up
    }

    // Surfaces a real promotion. The side comes from the event so the title matches
    // the ladder the player climbed.
    private void OnRankChanged(CivilWarSide side, int index) =>
        Debug.Notification($"You have been promoted to {CivilWarRank.RankTitle(side, index)}!");

    // Whether a hold's new owner is the side the player fights for.
    private static bool IsPlayersSide(Side captor) => captor switch
    {
        Side.Imperial => CivilWarState.CurrentSide == CivilWarSide.Imperial,
        Side.Stormcloak => CivilWarState.CurrentSide == CivilWarSide.Stormcloak,
        _ => false,
    };
}
