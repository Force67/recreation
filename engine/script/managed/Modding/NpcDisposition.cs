using Recreation;

namespace Recreation.Modding;

// How an NPC regards the player at a glance, the coarse reaction that gates
// dialogue, trade and aggression.
public enum Disposition
{
    Hostile = -1,
    Neutral = 0,
    Friendly = 1,
}

// The cross-game resolver other systems read to ask "how does this NPC see the
// player right now?". It composes the three signals the engine and the social
// layer expose, in priority order:
//
//   1. faction reaction  -- an Enemy faction pairing makes the NPC Hostile outright
//                           (the engine's own combat reaction wins)
//   2. wanted level      -- a Wanted/Hunted player turns a law-keeping NPC Hostile,
//                           and dents a neutral one to wary
//   3. relationship rank -- a personal bond (friend, rival) tips an otherwise
//                           neutral read warm or cold
//
// Pure and static: it holds no state, so it needs no lifecycle and is trivially
// testable. It mirrors Skyrim's FactionRelations but folds in the player's heat,
// the piece a single-game reaction cannot see. Other systems (GuardResponse, a
// future C++ AI) call Toward to decide whether to greet, ignore or draw on the
// player.
public static class NpcDisposition
{
    // The relationship rank at or above which a bond reads as Friendly, and at or
    // below which it reads as Hostile. On Skyrim's -4..4 rank scale a Friend (1)
    // is warm and a Foe (-2) is cold; 0 (Acquaintance) is neutral.
    public const int FriendlyRank = 1;
    public const int HostileRank = -2;

    // The wanted band at which a law-respecting NPC turns on the player. At or
    // above Wanted the guards (and most citizens) treat the player as a criminal.
    public const WantedBand HostileWantedBand = WantedBand.Wanted;

    // Resolves the NPC's disposition from the three signals. `factionReaction` is
    // how the NPC's factions regard the player's (Faction.ReactionToward, or a
    // game's FactionRelations), `relationshipRank` is their personal bond (0 when
    // strangers), and `wanted` is the player's current heat. `respectsLaw` lets a
    // bandit or a fellow outlaw ignore the wanted level the way a guard cannot.
    public static Disposition Toward(CombatReaction factionReaction, int relationshipRank,
                                     WantedBand wanted, bool respectsLaw = true)
    {
        // A hostile faction pairing dominates: the engine would have them fight.
        if (factionReaction == CombatReaction.Enemy) return Disposition.Hostile;

        // A law-keeper turns on a wanted player regardless of a warm faction or a
        // mild friendship; the crime overrides the goodwill.
        if (respectsLaw && wanted >= HostileWantedBand) return Disposition.Hostile;

        // Otherwise the personal bond decides, with an allied/friendly faction
        // nudging a stranger warm.
        if (relationshipRank >= FriendlyRank) return Disposition.Friendly;
        if (relationshipRank <= HostileRank) return Disposition.Hostile;
        if (factionReaction >= CombatReaction.Ally) return Disposition.Friendly;
        return Disposition.Neutral;
    }

    // The disposition of `observer` toward the player, reading the faction reaction
    // off the records and the heat off the live wanted meter. The convenience that
    // pulls the signals together for callers that have the actors in hand.
    public static Disposition Toward(Actor observer, Actor player, int relationshipRank,
                                     WantedLevel? wanted, bool respectsLaw = true) =>
        Toward(FactionReactionToward(observer, player), relationshipRank,
               wanted?.Band ?? WantedBand.Clear, respectsLaw);

    public static bool IsHostile(Disposition d) => d == Disposition.Hostile;
    public static bool IsFriendly(Disposition d) => d == Disposition.Friendly;

    // How `observer`'s factions regard `target`'s, the same Enemy-dominates,
    // friendliest-otherwise fold Skyrim's FactionRelations uses, lifted here so the
    // resolver works in any game without depending on a single game's layer.
    public static CombatReaction FactionReactionToward(Actor observer, Actor target)
    {
        CombatReaction best = CombatReaction.Neutral;
        foreach (FactionMembership a in observer.Factions)
            foreach (FactionMembership b in target.Factions)
            {
                CombatReaction reaction = a.Faction.ReactionToward(b.Faction);
                if (reaction == CombatReaction.Enemy) return CombatReaction.Enemy;
                if (reaction > best) best = reaction;
            }
        return best;
    }
}
