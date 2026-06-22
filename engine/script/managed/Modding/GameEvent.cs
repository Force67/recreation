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
public readonly struct QuestStageChanged(uint questFormId, int stage) : IGameEvent
{
    public uint QuestFormId { get; } = questFormId;
    public int Stage { get; } = stage;
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
