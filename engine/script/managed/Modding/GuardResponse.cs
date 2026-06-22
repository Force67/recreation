using System;
using Recreation;

namespace Recreation.Modding;

// Raised when an authority NPC confronts a wanted player, the "Halt! You've
// violated the law" moment. GuardResponse fires it once per encounter; combat,
// dialogue and the arrest flow hook it. Cleared (Confronting == false) when the
// heat drops and the guard stands down.
public readonly struct GuardConfrontation(ulong guardHandle, bool confronting) : IGameEvent
{
    public ulong GuardHandle { get; } = guardHandle;

    // True on the challenge, false when the standoff ends (the bounty was paid).
    public bool Confronting { get; } = confronting;

    public Actor Guard => Actor.From(GuardHandle);
}

// The confrontation layer over WantedLevel: while the player is wanted it watches
// for an authority NPC (a member of a crime-tracking faction) in range and, on the
// first one it sees, raises a GuardConfrontation and a "Halt!" notification. When
// the heat drops below the wanted band it stands the guard down with a closing
// event. It moves no one and starts no combat: it only turns "wanted + a guard is
// near" into the cue the AI and dialogue act on, the way the rest of the social
// layer stays data-only.
//
// Driven per frame but throttled to a scan interval (the proximity query is not
// free), and gated on the wanted band so it costs nothing while the player is
// clean.
public sealed class GuardResponse : GameBehaviour
{
    // The wanted meter this answers to. The standoff opens at Wanted and up.
    private readonly WantedLevel _wanted;

    public GuardResponse(WantedLevel wanted) =>
        _wanted = wanted ?? throw new ArgumentNullException(nameof(wanted));

    // How far (game units) a guard must be to confront, and how often (seconds) to
    // scan. The defaults match a guard hailing the player across a market square.
    public float ConfrontRadius { get; set; } = 600f;
    public float ScanInterval { get; set; } = 1f;

    // The guard currently confronting the player, 0 when none. Public so a test (or
    // the arrest flow) can read who is challenging.
    public ulong ActiveGuard { get; private set; }
    public bool Confronting => ActiveGuard != 0;

    private float _sinceScan;

    protected override void OnUpdate(float deltaTime)
    {
        _sinceScan += deltaTime;
        if (_sinceScan < ScanInterval) return;
        _sinceScan = 0f;
        Scan();
    }

    protected override void OnDestroy() => StandDown();

    // One scan pass: stand the guard down if the player is no longer wanted, else
    // open a standoff with the nearest authority NPC in range.
    private void Scan()
    {
        if (!_wanted.IsWanted)
        {
            StandDown();
            return;
        }
        if (Confronting) return;  // already in a standoff; hold it until paid

        Actor player = Game.Player;
        foreach (NearbyRef near in player.RefsNear(ConfrontRadius))
        {
            var actor = Actor.From(near.Reference.Handle);
            if (actor.IsDead || !IsAuthority(actor)) continue;
            Confront(actor);
            return;
        }
    }

    // Whether `actor` answers for the law: a member of a crime-tracking faction
    // (a hold guard, a UC vanguard). The same flag Skyrim's Crime keys off.
    private static bool IsAuthority(Actor actor)
    {
        foreach (FactionMembership m in actor.Factions)
            if (m.Faction.IsCrimeFaction) return true;
        return false;
    }

    private void Confront(Actor guard)
    {
        ActiveGuard = guard.Handle;
        EventBus.Publish(new GuardConfrontation(guard.Handle, true));
        Debug.Notification("Halt! You've committed crimes against the law.");
    }

    // Ends the active standoff (the player paid the bounty or fled out of heat).
    private void StandDown()
    {
        if (!Confronting) return;
        ulong guard = ActiveGuard;
        ActiveGuard = 0;
        EventBus.Publish(new GuardConfrontation(guard, false));
    }
}
