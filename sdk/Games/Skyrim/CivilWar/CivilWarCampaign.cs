using System;

namespace Recreation.Games.Skyrim;

// Skyrim's nine holds. Ordering is incidental; code keys on the value, never the
// index. The parenthetical is each hold's seat (the city the war is fought over).
public enum Hold
{
    Haafingar,   // Solitude  - Imperial capital
    TheReach,    // Markarth
    Falkreath,   // Falkreath
    Hjaalmarch,  // Morthal
    Whiterun,    // Whiterun  - neutral until the war forces a choice
    Eastmarch,   // Windhelm  - Stormcloak capital
    TheRift,     // Riften
    Winterhold,  // Winterhold
    ThePale,     // Dawnstar
}

// Who controls a hold. Distinct from CivilWarSide (the player's allegiance, which
// has no neutral state): a hold can sit unclaimed before the war reaches it.
public enum Side
{
    Neutral,
    Imperial,
    Stormcloak,
}

// The campaign board: which side holds each of Skyrim's nine holds, and the
// derived war progress. A pure, self-contained model the war readout reads and the
// campaign tracker drives; ownership only ever moves through CaptureHold, which the
// tracker calls on a Civil War siege/mission completing. No timers, no engine
// calls, no combat. The vanilla game never shows this state as one clean picture;
// this is the model behind a modern war-map readout.
public static class CivilWarCampaign
{
    // Skyrim has nine holds; war progress is a fraction of this.
    public const int TotalHolds = 9;

    // Hold -> controlling side, indexed by (int)Hold. Seeded to the canonical
    // war-start board and restored there by Reset.
    private static readonly Side[] Owners = BuildCanonicalBoard();

    // Raised after CaptureHold actually changes a hold's owner, carrying the hold
    // and its new side. The tracker listens to surface the capture to the player.
    public static event Action<Hold, Side>? HoldCaptured;

    // The canonical board at the war's outbreak: the Empire holds the west and
    // north-west, the Stormcloaks the east, Whiterun stays neutral. Four each.
    private static Side[] BuildCanonicalBoard()
    {
        var board = new Side[TotalHolds];
        board[(int)Hold.Haafingar] = Side.Imperial;
        board[(int)Hold.TheReach] = Side.Imperial;
        board[(int)Hold.Falkreath] = Side.Imperial;
        board[(int)Hold.Hjaalmarch] = Side.Imperial;
        board[(int)Hold.Eastmarch] = Side.Stormcloak;
        board[(int)Hold.TheRift] = Side.Stormcloak;
        board[(int)Hold.Winterhold] = Side.Stormcloak;
        board[(int)Hold.ThePale] = Side.Stormcloak;
        board[(int)Hold.Whiterun] = Side.Neutral;
        return board;
    }

    public static Side OwnerOf(Hold hold) => Owners[(int)hold];

    // How many holds a side controls.
    public static int HoldCount(Side side)
    {
        int n = 0;
        foreach (Side owner in Owners)
            if (owner == side) n++;
        return n;
    }

    public static int ImperialHoldCount => HoldCount(Side.Imperial);
    public static int StormcloakHoldCount => HoldCount(Side.Stormcloak);

    // A side's share of all nine holds, 0..1. The single number a war-progress
    // bar tracks; the two warring sides' shares plus the neutral share sum to 1.
    public static float WarProgress(Side side) => HoldCount(side) / (float)TotalHolds;

    // Hands a hold to a side. Returns true (and raises HoldCaptured) only when this
    // is a real change; recapturing a hold the side already controls is a no-op, so
    // a quest that re-fires its completion stage does not double-toast.
    public static bool CaptureHold(Hold hold, Side side)
    {
        if (Owners[(int)hold] == side) return false;
        Owners[(int)hold] = side;
        HoldCaptured?.Invoke(hold, side);
        return true;
    }

    // Advances the front for `attacker`: takes the next vulnerable hold, the
    // contested seat (Whiterun) first, then the nearest enemy-held hold, and
    // hands it over. The campaign rule behind "win a battle, gain a hold": the
    // engine reports a siege won, this decides which hold falls (and raises
    // HoldCaptured, so the war map and rank react). Returns the captured hold, or
    // null when the attacker already holds everything it can take.
    public static Hold? AdvanceFront(Side attacker)
    {
        if (attacker == Side.Neutral) return null;
        Side enemy = attacker == Side.Imperial ? Side.Stormcloak : Side.Imperial;
        if (OwnerOf(Hold.Whiterun) == Side.Neutral && CaptureHold(Hold.Whiterun, attacker))
            return Hold.Whiterun;
        for (int i = 0; i < TotalHolds; i++)
        {
            var hold = (Hold)i;
            if (OwnerOf(hold) == enemy && CaptureHold(hold, attacker))
                return hold;
        }
        return null;
    }

    // The seat of a hold, for readouts that name the city the battle was for.
    public static string CityOf(Hold hold) => hold switch
    {
        Hold.Haafingar => "Solitude",
        Hold.TheReach => "Markarth",
        Hold.Falkreath => "Falkreath",
        Hold.Hjaalmarch => "Morthal",
        Hold.Whiterun => "Whiterun",
        Hold.Eastmarch => "Windhelm",
        Hold.TheRift => "Riften",
        Hold.Winterhold => "Winterhold",
        Hold.ThePale => "Dawnstar",
        _ => hold.ToString(),
    };

    // Restores the canonical war-start board. Called on teardown so a reload starts
    // the war fresh.
    public static void Reset()
    {
        Side[] canonical = BuildCanonicalBoard();
        Array.Copy(canonical, Owners, TotalHolds);
    }
}
