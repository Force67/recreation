using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Grants the player character experience as quests advance, the way Fallout 4 pays
// XP for objective progress. It subscribes to the engine's QuestStageChanged, which
// fires for local and multiplayer-replicated stage changes alike, and rewards each
// quest/stage milestone only once so a re-entered or re-replicated stage cannot
// farm XP. Feeds CharacterProgress, so quest progress drives leveling and the perk
// points it grants, tying the questing layer to the gameplay loop. The Fallout twin
// of StarfieldQuestRewards.
public sealed class FalloutQuestRewards : GameBehaviour
{
    // Character XP granted the first time a quest reaches a given stage.
    public float XpPerStage { get; set; } = 50f;

    private readonly HashSet<(ulong Quest, int Stage)> _rewarded = new();
    private EventBus.Subscription? _sub;

    protected override void OnStart() => _sub = EventBus.Subscribe<QuestStageChanged>(OnStage);

    protected override void OnDestroy() => _sub?.Dispose();

    private void OnStage(QuestStageChanged e)
    {
        if (!_rewarded.Add((e.QuestHandle, e.Stage))) return;  // milestone already paid out
        CharacterProgress.GainXp(Game.Player, XpPerStage);
    }
}
