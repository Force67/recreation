using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Advances the war-map front when a real Civil War battle quest is won in the
// world. The engine drives the OG siege quest to its completion stage (the
// player having fought the battle); this reacts to that and hands the next hold
// to the player's side, so a won siege visibly moves the campaign board. It keys
// on the real Skyrim.esm quest ids (unlike the placeholder hold->mission table),
// and feeds CivilWarCampaign, which the war map and rank tracker already follow.
public sealed class CivilWarFrontline : GameBehaviour
{
    // Real Civil War battle quests and the stage that marks the win.
    private static readonly Dictionary<uint, int> WonAt = new()
    {
        [0x00083042] = 9000,  // CWFortSiegeFort, the fort siege
        [0x0002b272] = 200,   // CWResolution01, attack the enemy capital
    };

    private EventBus.Subscription? _sub;

    protected override void OnStart() => _sub = EventBus.Subscribe<QuestStageChanged>(OnStage);

    protected override void OnDestroy() => _sub?.Dispose();

    private void OnStage(QuestStageChanged e)
    {
        if (!WonAt.TryGetValue(e.Quest.FormId, out int won) || e.Stage < won) return;
        // The player's side takes the hold; an unaligned demo defaults to the Legion.
        Side attacker = CivilWarState.CurrentSide == CivilWarSide.Stormcloak
            ? Side.Stormcloak
            : Side.Imperial;
        CivilWarCampaign.AdvanceFront(attacker);  // -> HoldCaptured -> war map + rank
    }
}
