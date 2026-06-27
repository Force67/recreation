using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Pushes the Civil War campaign board onto the engine's war-map panel: every
// hold's controlling side and the overall war progress. It mirrors the
// CivilWarCampaign model to the engine on start and whenever a hold changes
// hands, so opening the war map (M) shows the live front. Read-only over the
// campaign; it owns no state of its own.
public sealed class WarMapPusher : GameBehaviour
{
    protected override void OnStart()
    {
        CivilWarCampaign.HoldCaptured += OnHoldCaptured;
        Push();  // seed the panel with the opening board
    }

    protected override void OnDestroy() => CivilWarCampaign.HoldCaptured -= OnHoldCaptured;

    private void OnHoldCaptured(Hold hold, Side side) => Push();

    // Mirror every hold + the war progress to the engine.
    private static void Push()
    {
        for (int i = 0; i < CivilWarCampaign.TotalHolds; i++)
        {
            var hold = (Hold)i;
            WarMap.SetHold(i, CivilWarCampaign.CityOf(hold), (int)CivilWarCampaign.OwnerOf(hold));
        }
        WarMap.SetProgress(CivilWarCampaign.WarProgress(Side.Imperial));
    }
}
