namespace Recreation.Modding;

// A Unity-flavoured behaviour: a unit of mod logic with a managed lifecycle.
// Derive from this and override the hooks you need; the mod host discovers the
// type, instantiates it once, and drives it:
//
//   OnStart   once, after construction, when the world is ready
//   OnUpdate  every frame, while Enabled, with the frame delta in seconds
//   OnDestroy once, when the managed world tears down
//
// This is the entry most mods want: a place to run code each frame and react to
// the game, without touching the native boundary directly.
public abstract class GameBehaviour
{
    // While false, OnUpdate is skipped. Toggle to pause a behaviour without
    // unregistering it.
    public bool Enabled { get; set; } = true;

    // True once OnStart has run; the host sets it.
    public bool Started { get; private set; }

    protected virtual void OnStart() { }
    protected virtual void OnUpdate(float deltaTime) { }
    protected virtual void OnDestroy() { }

    // Lifecycle entry points the mod host calls. Kept internal so only the host
    // drives the order; mod code overrides the protected hooks above.
    internal void DispatchStart()
    {
        if (Started) return;
        Started = true;
        OnStart();
    }

    internal void DispatchUpdate(float deltaTime)
    {
        if (Enabled && Started) OnUpdate(deltaTime);
    }

    internal void DispatchDestroy()
    {
        if (Started) OnDestroy();
    }
}
