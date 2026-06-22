using Recreation;

namespace Recreation.Modding;

// Whether an NPC will wade into the player's fight, on Skyrim's assistance scale.
public enum Assistance
{
    HelpsNobody = 0,        // never joins a fight
    HelpsFriendsAndAllies = 1,  // joins for friends and allied factions
    HelpsAllies = 2,        // joins for anyone allied (the readiest helper)
}

// The cross-game resolver for "will this NPC fight at my side?", the morale half of
// the disposition layer. It folds the NPC's standing assistance flag together with
// the live bond: a HelpsAllies NPC backs the player whenever they are not on the
// outs, a HelpsFriendsAndAllies NPC needs a real friendship or an allied faction,
// and a HelpsNobody NPC stays out of it. A hostile bond or an enemy faction vetoes
// help outright, however eager the flag.
//
// Pure and static, mirroring NpcDisposition: no state, no lifecycle, trivially
// tested. The combat AI calls WillAssist to decide whether to pull a nearby ally
// into the player's fight.
public static class NpcMorale
{
    // The relationship rank at or above which a bond counts as a real friendship
    // (Skyrim's Friend), and at or below which it vetoes help (Foe). Matches
    // NpcDisposition's thresholds so the two layers agree.
    public const int FriendRank = NpcDisposition.FriendlyRank;
    public const int HostileRank = NpcDisposition.HostileRank;

    // Whether an NPC with `assistance` aid, `factionReaction` toward the player and
    // `relationshipRank` with them will join the player's fight.
    public static bool WillAssist(Assistance assistance, CombatReaction factionReaction,
                                  int relationshipRank)
    {
        // A hostile bond or an enemy faction never helps, whatever the flag says.
        if (factionReaction == CombatReaction.Enemy || relationshipRank <= HostileRank)
            return false;

        return assistance switch
        {
            Assistance.HelpsNobody => false,
            // Helps the allied: anyone not on the outs (handled above) qualifies.
            Assistance.HelpsAllies => true,
            // Helps friends and allies: needs a real friendship or an allied/friendly
            // faction, not a bare neutral acquaintance.
            Assistance.HelpsFriendsAndAllies =>
                relationshipRank >= FriendRank || factionReaction >= CombatReaction.Ally,
            _ => false,
        };
    }

    // The assistance of `helper` toward the player, reading the faction reaction
    // off the records. The convenience for callers holding the actors.
    public static bool WillAssist(Actor helper, Actor player, Assistance assistance,
                                  int relationshipRank) =>
        WillAssist(assistance, NpcDisposition.FactionReactionToward(helper, player), relationshipRank);
}
