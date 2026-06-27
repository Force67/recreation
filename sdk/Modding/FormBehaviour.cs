namespace Recreation.Modding;

// A behaviour attached to one specific form, the managed analog of a Papyrus
// script on an object or a Unity component on a GameObject. It carries the form
// it runs on (Self) and turns the form-targeted engine events into clean hooks,
// so a mod reacts to "this actor died" or "an item entered this container"
// without filtering the global bus by hand.
//
// Override the hooks you need: OnAttach/OnDetach for setup and teardown, OnUpdate
// (from GameBehaviour) for per-frame work, OnDeath and OnItemAdded for the routed
// events. The base wires and unwires the subscriptions, so the start/destroy
// lifecycle is sealed; use OnAttach/OnDetach instead.
public abstract class FormBehaviour : GameBehaviour
{
    // The form this behaviour runs on. Set by FormScripts.Attach before start.
    public Form Self { get; internal set; } = Form.None;

    // Self viewed as an actor, for the common actor case.
    public Actor SelfActor => Actor.From(Self.Handle);

    private EventBus.Subscription? _died;
    private EventBus.Subscription? _itemAdded;

    protected sealed override void OnStart()
    {
        _died = EventBus.Subscribe<ActorDied>(e =>
        {
            if (e.ActorHandle == Self.Handle) OnDeath();
        });
        _itemAdded = EventBus.Subscribe<ItemAdded>(e =>
        {
            if (e.ContainerHandle == Self.Handle) OnItemAdded(e.Item, e.Count);
        });
        OnAttach();
    }

    protected sealed override void OnDestroy()
    {
        _died?.Dispose();
        _itemAdded?.Dispose();
        OnDetach();
    }

    // Setup and teardown for the attached form.
    protected virtual void OnAttach() { }
    protected virtual void OnDetach() { }

    // Self died (its health reached zero).
    protected virtual void OnDeath() { }

    // An item entered Self's inventory.
    protected virtual void OnItemAdded(Form item, int count) { }
}
