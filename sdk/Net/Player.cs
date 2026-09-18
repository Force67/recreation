using Recreation.Interop;

namespace Recreation.Net;

// A participant in the session, addressed by its stable peer id. A thin handle;
// its data lives in its replicated state bag.
public sealed class Player
{
    // Reserved keys the platform manages inside a player's state bag.
    internal const string PresentKey = "present";
    internal const string NameKey = "name";
    internal const string ActorKey = "actor";

    public uint Id { get; }

    internal Player(uint id) => Id = id;

    // This player's replicated state bag. Server-authoritative, except a client may
    // write its own player bag.
    public StateBag State => StateBags.Player(Id);

    // True when this is the player this machine controls.
    public bool IsLocal => Id == Players.LocalId;

    // In the roster (its presence has replicated to this machine).
    public bool Present => State.Get(PresentKey).AsBool();

    // The display name, falling back to a stable placeholder until one is set.
    public string Name
    {
        get
        {
            string n = State.Get(NameKey).AsString();
            return string.IsNullOrEmpty(n) ? $"Player {Id}" : n;
        }
    }

    // The engine handle (actor/form) this player drives, 0 until one is assigned.
    public ulong Actor => State.Get(ActorKey).AsHandle();

    // Name this player. On a client this writes its own bag; on the server a mod names anyone.
    public void SetName(string name) => State.Set(NameKey, name);

    // Bind a gameplay actor/form to this player. Server-authoritative.
    public void SetActor(ulong handle) => State.Set(ActorKey, Value.Object(handle));

    // --- Replicated vitals (server-authoritative; the engine combat system is
    // not networked yet, so today the producer is host-side mod code). Setting
    // broadcasts a kPlayerState to every client, where it lands on the player's
    // replica entity as a PlayerVitals component and a PlayerVitalsChanged
    // managed event; the dead flag also drives the client-side Dead tag. ---

    // Sets this player's health and ceiling, and whether they are down. Call on
    // the server; every client sees the result. Unchanged values stay off the
    // wire.
    public void SetHealth(int health, int maxHealth, bool dead = false)
    {
        Native.CallGlobal("Net", "SetPlayerHealth",
                          new[] { Value.Int((int)Id), Value.Int(health), Value.Int(maxHealth),
                                  Value.Bool(dead) });
    }

    public override string ToString() => $"{Name}#{Id}";
}
