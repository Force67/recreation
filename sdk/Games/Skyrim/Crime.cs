namespace Recreation.Games.Skyrim;

// Skyrim's crime and bounty: each hold has a crime faction that tracks the
// player's bounty there, and an NPC reports crimes to the crime faction it
// belongs to. Spotting a crime is the engine's (or a mod's) job; the bounty
// ledger -- find the hold, add a bounty, query it, pay it off -- is soft logic
// here over faction membership and the crime-gold store.
public static class Crime
{
    // The crime faction an actor reports to (its crime-flagged membership), or a
    // null Faction if it has none (most creatures and many NPCs).
    public static Faction FactionOf(Actor actor)
    {
        foreach (FactionMembership membership in actor.Factions)
            if (membership.Faction.IsCrimeFaction) return membership.Faction;
        return Faction.From(0);
    }

    // Adds `bounty` to a hold's crime faction.
    public static void Report(Faction crimeFaction, int bounty)
    {
        if (crimeFaction.Exists) crimeFaction.ModCrimeGold(bounty);
    }

    // Reports a crime to the hold the `witness` answers to (a no-op if it has no
    // crime faction).
    public static void Report(Actor witness, int bounty) => Report(FactionOf(witness), bounty);

    // The current bounty in a hold.
    public static int BountyIn(Faction crimeFaction) => crimeFaction.CrimeGold;

    // Whether the player is wanted in the hold.
    public static bool IsWanted(Faction crimeFaction) => crimeFaction.Exists && crimeFaction.CrimeGold > 0;

    // Clears the hold's bounty (the player paid the fine or served the time).
    public static void Pay(Faction crimeFaction)
    {
        if (crimeFaction.Exists) crimeFaction.CrimeGold = 0;
    }
}
