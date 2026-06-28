namespace Recreation.Modding;

// Marker for anything that can travel through the EventBus. Events are small
// immutable messages; mods define their own by implementing this, and the
// engine raises the built-in ones below.
public interface IGameEvent
{
}

// Raised once per frame, carrying the elapsed time. The Unity-style OnUpdate of
// a GameBehaviour is driven by this; mods can also subscribe directly.
public readonly struct FrameUpdate(float deltaTime) : IGameEvent
{
    public float DeltaTime { get; } = deltaTime;
}

// Raised when a quest reaches a new stage. The engine publishes these so mods
// react to story progression without owning the quest.
public readonly struct QuestStageChanged(ulong questHandle, int stage) : IGameEvent
{
    public ulong QuestHandle { get; } = questHandle;
    public int Stage { get; } = stage;

    public Quest Quest => Quest.From(QuestHandle);
}

// Raised when an actor dies.
public readonly struct ActorDied(ulong actorHandle) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;

    public Actor Actor => Actor.From(ActorHandle);
}

// Raised when an item enters a container's inventory (item added).
public readonly struct ItemAdded(ulong containerHandle, ulong itemHandle, int count) : IGameEvent
{
    public ulong ContainerHandle { get; } = containerHandle;
    public ulong ItemHandle { get; } = itemHandle;
    public int Count { get; } = count;

    public ObjectReference Container => ObjectReference.From(ContainerHandle);
    public Form Item => Form.From(ItemHandle);
}

// Raised when a form goes live in the world (its scripts attach as it streams
// in). The hook a mod uses to attach its own behaviour to matching forms, the
// analog of gmod's OnEntityCreated.
public readonly struct FormLoaded(ulong formHandle) : IGameEvent
{
    public ulong FormHandle { get; } = formHandle;

    public Form Form => Form.From(FormHandle);
    public ObjectReference Reference => ObjectReference.From(FormHandle);
    public Actor Actor => Actor.From(FormHandle);
}

// Raised when the player activates (uses) a reference, the analog of gmod's
// PlayerUse. Mods react to the player interacting with the world.
public readonly struct PlayerActivated(ulong targetHandle) : IGameEvent
{
    public ulong TargetHandle { get; } = targetHandle;

    public ObjectReference Target => ObjectReference.From(TargetHandle);
}
