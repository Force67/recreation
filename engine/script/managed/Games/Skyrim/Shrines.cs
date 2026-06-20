using System.Collections.Generic;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when a timed blessing runs out on its own.
public readonly struct BlessingExpired(ulong actorHandle, ulong blessingHandle) : IGameEvent
{
    public ulong ActorHandle { get; } = actorHandle;
    public ulong BlessingHandle { get; } = blessingHandle;

    public Spell Blessing => Spell.From(BlessingHandle);
}

// Timed blessings: a shrine's gift, an ability an actor holds for a span of game
// time rather than forever. A character holds one at a time (a new prayer replaces
// the old), and it lapses on its own once its hours run out. Built on the ability
// system for the held effects, with expiry measured against the game clock so it
// is unaffected by how fast real time runs. Persistent player state; tests clear
// it. The BlessingUpkeep system lets it lapse each frame.
public static class Blessings
{
    private readonly record struct Held(ulong Spell, float Expiry);  // expiry in game days
    private static readonly Dictionary<ulong, Held> Active = new();

    // Grants `blessing` to `actor` for `gameHours`, replacing any blessing held.
    public static void Grant(Actor actor, Spell blessing, float gameHours)
    {
        if (Active.Remove(actor.Handle, out Held prev))
            Abilities.Remove(actor, Spell.From(prev.Spell));  // replaced, not expired
        Abilities.Apply(actor, blessing);
        Active[actor.Handle] = new Held(blessing.Handle, GameClock.GameTime + gameHours / 24f);
    }

    // The blessing the actor currently holds, or 0 if none (after lapsing any that
    // have run out).
    public static ulong Current(Actor actor)
    {
        Refresh();
        return Active.TryGetValue(actor.Handle, out Held held) ? held.Spell : 0;
    }

    public static bool Holds(Actor actor, Spell blessing) => Current(actor) == blessing.Handle;

    // Lifts every blessing whose time has run out. The upkeep system calls this
    // each frame so a blessing fades on its own; queries call it too.
    public static void Refresh()
    {
        float now = GameClock.GameTime;
        List<ulong>? lapsed = null;
        foreach (KeyValuePair<ulong, Held> entry in Active)
            if (entry.Value.Expiry <= now) (lapsed ??= new List<ulong>()).Add(entry.Key);
        if (lapsed == null) return;
        foreach (ulong actor in lapsed)
        {
            Held held = Active[actor];
            Active.Remove(actor);
            Abilities.Remove(Actor.From(actor), Spell.From(held.Spell));
            EventBus.Publish(new BlessingExpired(actor, held.Spell));
        }
    }

    public static void Clear() => Active.Clear();
}

// The result of praying at a shrine: the blessing taken and how many diseases the
// prayer washed away.
public readonly struct Prayer(ulong blessing, int diseasesCured)
{
    public ulong Blessing { get; } = blessing;
    public int DiseasesCured { get; } = diseasesCured;
}

// A shrine of the Divines: praying at one cures all disease and grants its blessing
// for a span of game time. Soft logic over the disease and blessing systems, so a
// mod stands up a working shrine by calling Pray from its activator.
public static class Shrine
{
    // How long a shrine blessing lasts in vanilla.
    public const float BlessingHours = 8f;

    public static Prayer Pray(Actor worshipper, Spell blessing, float hours = BlessingHours)
    {
        int cured = Diseases.Cure(worshipper);
        Blessings.Grant(worshipper, blessing, hours);
        return new Prayer(blessing.Handle, cured);
    }
}

// Lapses timed blessings on their own: each frame it lets the blessing registry
// drop any whose time has run out, so a shrine's gift fades without the shrine or
// the player doing anything.
public sealed class BlessingUpkeep : GameBehaviour
{
    protected override void OnUpdate(float deltaTime) => Blessings.Refresh();
}
