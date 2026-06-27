namespace Recreation;

// A quest (QUST record): a staged objective graph with running and active
// state. Mod code drives quests by setting stages; the engine runs the stage's
// fragments and updates the journal.
public class Quest : Form
{
    protected Quest(ulong handle) : base(handle) { }

    public static new Quest From(ulong handle) => new(handle);

    // The current stage id. Setting it runs the stage's fragment and advances
    // the journal, the same as Papyrus SetStage.
    public int Stage
    {
        get => Call("GetStage").AsInt();
        set => Call("SetStage", value);
    }

    public bool IsStageDone(int stage) => Call("GetStageDone", stage).AsBool();

    // Journal (CNAM) text of the most recently set stage. Empty for a control
    // stage with no log entry, so it doubles as "did this stage update the
    // journal?" for code that surfaces progress to the player.
    public string JournalEntry => Call("GetJournalEntry").AsString();

    public bool IsRunning => Call("IsRunning").AsBool();
    public bool IsActive
    {
        get => Call("IsActive").AsBool();
        set => Call("SetActive", value);
    }

    public void Start() => Call("Start");
    public void Stop() => Call("Stop");
    public void Reset() => Call("Reset");

    // --- objectives -----------------------------------------------------------
    public void SetObjectiveDisplayed(int objective, bool displayed = true) =>
        Call("SetObjectiveDisplayed", objective, displayed);
    public void SetObjectiveCompleted(int objective, bool completed = true) =>
        Call("SetObjectiveCompleted", objective, completed);
    public bool IsObjectiveDisplayed(int objective) =>
        Call("IsObjectiveDisplayed", objective).AsBool();
    public bool IsObjectiveCompleted(int objective) =>
        Call("IsObjectiveCompleted", objective).AsBool();
}
