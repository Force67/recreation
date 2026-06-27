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

        AutoAttachOnFormLoaded(check);
        AutoDetachOnFormUnloaded(check);
    }

    // The gmod-style pattern: a mod subscribes to FormLoaded and attaches its own
    // behaviour to each matching form as it streams in.
    private static void AutoAttachOnFormLoaded(Check check)
    {
        ModHost.Shutdown();
        EventBus.Clear();

        using var sub = EventBus.Subscribe<FormLoaded>(e => FormScripts.Attach<GuardScript>(e.Form));

        EngineEvents.Dispatch(new Interop.ManagedEvent
        {
            Id = Interop.ManagedEventId.FormLoaded,
            A = 0x500,
        });
        check.Equal("behaviour auto-attached on form load", 1, FormScripts.Count);

        // The attached behaviour now receives that form's events.
        var attached = (GuardScript)FormScripts.For(Form.From(0x500))[0];
        EventBus.Publish(new ActorDied(0x500));
        check.Equal("auto-attached behaviour gets its form's events", 1, attached.Deaths);

        ModHost.Shutdown();
        EventBus.Clear();
    }

    // The symmetric half ModHost wires at boot: a form unloading detaches the
    // behaviours on it, the OnDestroy side of the lifecycle. Mirrors ModHost's
    // FormUnloaded -> FormScripts.DetachAll subscription so it runs without the
    // full mod-discovery boot.
    private static void AutoDetachOnFormUnloaded(Check check)
    {
        ModHost.Shutdown();
        EventBus.Clear();

        using var sub = EventBus.Subscribe<FormUnloaded>(e => FormScripts.DetachAll(e.Form));

        GuardScript guard = FormScripts.Attach<GuardScript>(Form.From(0x600));
        FormScripts.Attach<GuardScript>(Form.From(0x601));
        check.Equal("two forms attached", 2, FormScripts.Count);

        EngineEvents.Dispatch(new Interop.ManagedEvent
        {
            Id = Interop.ManagedEventId.FormUnloaded,
            A = 0x600,
        });
        check.Equal("unload detaches the form's behaviour", 1, FormScripts.Count);
        check.Equal("unload runs OnDetach", 1, guard.Detached);

        // An unrelated form unloading leaves other attachments intact.
        EngineEvents.Dispatch(new Interop.ManagedEvent
        {
            Id = Interop.ManagedEventId.FormUnloaded,
            A = 0x999,
        });
        check.Equal("unrelated unload keeps attachments", 1, FormScripts.Count);

        ModHost.Shutdown();
        EventBus.Clear();
    }
}
