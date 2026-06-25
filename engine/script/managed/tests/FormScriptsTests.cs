using Recreation;
using Recreation.Modding;

namespace Recreation.Tests;

// A per-form behaviour that records the form-targeted hooks it receives.
public sealed class GuardScript : FormBehaviour
{
    public int Attached;
    public int Detached;
    public int Deaths;
    public int ItemsReceived;

    protected override void OnAttach() => Attached++;
    protected override void OnDetach() => Detached++;
    protected override void OnDeath() => Deaths++;
    protected override void OnItemAdded(Form item, int count) => ItemsReceived += count;
}

public static class FormScriptsTests
{
    public static void Run(Check check)
    {
        ModHost.Shutdown();
        EventBus.Clear();

        var actor = Form.From(0x100);
        var other = Form.From(0x200);

        GuardScript guard = FormScripts.Attach<GuardScript>(actor);
        check.Equal("attach runs OnAttach", 1, guard.Attached);
        check.Equal("self is the form", 0x100UL, guard.Self.Handle);
        check.Equal("registry tracks one", 1, FormScripts.Count);

        // An event for this form reaches its hook.
        EventBus.Publish(new ActorDied(0x100));
        check.Equal("OnDeath fires for self", 1, guard.Deaths);

        // An event for a different form does not.
        EventBus.Publish(new ActorDied(0x200));
        check.Equal("OnDeath ignores other forms", 1, guard.Deaths);

        // Item routing by container handle.
        EventBus.Publish(new ItemAdded(0x100, 0xAAA, 3));
        EventBus.Publish(new ItemAdded(0x200, 0xAAA, 5));
        check.Equal("OnItemAdded fires for self only", 3, guard.ItemsReceived);

        // Per-frame updates still flow through the behaviour lifecycle.
        FormScripts.Attach<GuardScript>(other);
        check.Equal("registry tracks both", 2, FormScripts.Count);

        FormScripts.DetachAll(actor);
        check.Equal("detach runs OnDetach", 1, guard.Detached);
        check.Equal("registry drops the form", 1, FormScripts.Count);

        // After detach, its hooks no longer fire.
        EventBus.Publish(new ActorDied(0x100));
        check.Equal("detached behaviour is silent", 1, guard.Deaths);

        ModHost.Shutdown();
        check.Equal("shutdown clears the registry", 0, FormScripts.Count);
        EventBus.Clear();
    }
}
