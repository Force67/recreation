namespace Recreation;

// A scene (SCEN record): a scripted, phased sequence of actor actions owned by
// a quest. Starting a scene plays its phases over time, firing the fragments
// that advance the owning quest.
public class Scene : Form
{
    protected Scene(ulong handle) : base(handle) { }

    public static new Scene From(ulong handle) => new(handle);

    public Quest OwningQuest => Quest.From(Call("GetOwningQuest").AsHandle());

    public void Start() => Call("Start");
    public void ForceStart() => Call("ForceStart");
    public void Stop() => Call("Stop");
    public bool IsPlaying => Call("IsPlaying").AsBool();
}
