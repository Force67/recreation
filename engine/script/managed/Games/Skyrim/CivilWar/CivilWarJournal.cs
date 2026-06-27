using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// The war journal: a polished toast when the Civil War story advances. It follows
// the QuestProgressTracker idiom (subscribe to QuestStageChanged, surface only
// stages that carry journal text), but narrows to Civil War quests and frames the
// update as a war-journal entry instead of the generic quest toast.
public sealed class CivilWarJournal : GameBehaviour
{
    private EventBus.Subscription? _subscription;

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<QuestStageChanged>(OnStageChanged);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnStageChanged(QuestStageChanged e)
    {
        Quest quest = e.Quest;
        if (!CivilWarForms.IsCivilWarQuest(quest.FormId)) return;

        // Only log entries reach the player; silent control stages leave the
        // journal text empty and raise no toast, exactly as vanilla does.
        if (quest.JournalEntry.Length == 0) return;

        string name = quest.Name;
        Debug.Notification(name.Length > 0
            ? $"War journal updated: {name}"
            : "War journal updated");
    }
}
