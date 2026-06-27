using System.Collections.Generic;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Starfield;

// Raised when the player's bounty in a jurisdiction changes (a crime reported, or
// the bounty paid down).
public readonly struct BountyChanged(uint jurisdiction, int bounty) : IGameEvent
{
    public uint Jurisdiction { get; } = jurisdiction;
    public int Bounty { get; } = bounty;
}

// Starfield's crime is jurisdictional: a bounty is owed to the faction whose space
// the crime happened in (the UC in Sol/Alpha Centauri, the Freestar Collective in
// the Freestar systems, the Crimson Fleet for piracy). The same idea as Skyrim's
// per-hold bounty, but keyed by jurisdiction faction rather than a crime-flagged
// membership, and held here as a managed ledger since these factions carry no
// engine crime-gold store. A witness or a watching system calls Report; the player
// clears it at a self-service kiosk (Pay) or wipes it whole (Clear bounty). A
// reported crime also costs standing with that faction. Persistent player state;
// tests reset it.
public static class Bounties
{
    private static readonly Dictionary<uint, int> Ledger = new();

    // How much reputation a reported crime costs with the offended faction. Scaled
    // to the bounty so a petty fine barely registers and a massacre is felt.
    public static float ReputationPenaltyPerBounty { get; set; } = 0.02f;

    public static int BountyIn(uint jurisdiction) => Ledger.GetValueOrDefault(jurisdiction);

    public static bool IsWanted(uint jurisdiction) => BountyIn(jurisdiction) > 0;

    // Adds to the player's bounty in a jurisdiction and docks standing with it. A
    // non-positive bounty is a no-op (a crime no one is fined for).
    public static void Report(uint jurisdiction, int bounty)
    {
        if (bounty <= 0) return;
        int updated = BountyIn(jurisdiction) + bounty;
        Ledger[jurisdiction] = updated;
        FactionReputation.Lose(jurisdiction, bounty * ReputationPenaltyPerBounty);
        EventBus.Publish(new BountyChanged(jurisdiction, updated));
    }

    // Pays `credits` against the bounty from the player's account, capped at what is
    // owed. Returns the amount actually paid (0 when nothing is owed or unaffordable).
    public static int Pay(Actor payer, uint jurisdiction, int credits)
    {
        int owed = BountyIn(jurisdiction);
        if (owed <= 0 || credits <= 0) return 0;
        int amount = credits < owed ? credits : owed;
        if (!Economy.Spend(payer, amount)) return 0;

        int updated = owed - amount;
        Ledger[jurisdiction] = updated;
        EventBus.Publish(new BountyChanged(jurisdiction, updated));
        return amount;
    }

    // Wipes a jurisdiction's bounty outright (served time, a pardon).
    public static void Forgive(uint jurisdiction)
    {
        if (BountyIn(jurisdiction) == 0) return;
        Ledger[jurisdiction] = 0;
        EventBus.Publish(new BountyChanged(jurisdiction, 0));
    }

    public static void Clear() => Ledger.Clear();
}
