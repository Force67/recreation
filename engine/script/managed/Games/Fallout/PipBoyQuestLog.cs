using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Surfaces quest progress to the player through the Pip-Boy, the Fallout 4 take on
// Skyrim's QuestProgressTracker. It subscribes to QuestStageChanged (local and
// multiplayer-replicated stage changes alike) and raises a short notification for
// stages that carry journal text, leaving silent control stages quiet. Pure
// event-driven managed logic; nothing here polls.
public sealed class PipBoyQuestLog : GameBehaviour
{
    private EventBus.Subscription? _subscription;

    // The most recent quest/stage transition observed, for other systems to read.
    public ulong LastQuestHandle { get; private set; }
    public int LastStage { get; private set; } = -1;

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<QuestStageChanged>(OnStageChanged);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnStageChanged(QuestStageChanged e)
    {
        LastQuestHandle = e.QuestHandle;
        LastStage = e.Stage;
        Quest quest = Quest.From(e.QuestHandle);
        if (quest.JournalEntry.Length > 0)
        {
            string name = quest.Name;
            Debug.Notification(name.Length > 0 ? $"{name} updated" : "Pip-Boy updated");
        }
    }
}
