using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a character takes a new standing-stone blessing: the blessing given
// up (0 if none) and the one taken.
public readonly struct StandingStoneChanged(ulong actorHandle, ulong previous, ulong taken) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public ulong Previous { get; } = previous;
    public ulong Taken { get; } = taken;
}

// The Standing Stones: wayside stones (The Warrior, The Mage, The Thief, The Lord,
// ...) that each grant one passive blessing. A character holds exactly one at a
// time, so touching a new stone trades the old blessing for the new. This layer
// owns that one-at-a-time rule and remembers each character's choice; the blessing
// itself is an ability, which fortifies the actor's values for as long as it is
// held. Persistent player state, so it outlives the mod-host lifecycle; tests
// clear it.
public static class StandingStones
{
    private static readonly Dictionary<ulong, ulong> Active = new();  // actor -> blessing spell

    // The blessing the actor currently holds, or 0 if they hold none.
    public static ulong Current(Actor actor) => Active.GetValueOrDefault(actor.Handle);

    public static bool Holds(Actor actor, Spell blessing) =>
        Active.GetValueOrDefault(actor.Handle) == blessing.Handle;

    // Touches a standing stone: trades whatever blessing the actor holds for
    // `blessing`. Returns false (changing nothing) when they already hold it, so
    // re-touching the same stone is a no-op, the way the game treats it.
    public static bool Activate(Actor actor, Spell blessing)
    {
        ulong previous = Active.GetValueOrDefault(actor.Handle);
        if (previous == blessing.Handle) return false;

        if (previous != 0) Abilities.Remove(actor, Spell.From(previous));
        Abilities.Apply(actor, blessing);
        Active[actor.Handle] = blessing.Handle;
        EventBus.Publish(new StandingStoneChanged(actor.Handle, previous, blessing.Handle));
        return true;
    }

    public static void Clear() => Active.Clear();
}
