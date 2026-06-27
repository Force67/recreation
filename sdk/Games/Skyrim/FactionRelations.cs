using System.Collections.Generic;

namespace Recreation.Games.Skyrim;

// Whether two actors get along, derived from their factions' reactions. The engine
// decides who fights whom from authored faction relations; this reads the same
// data so a mod can ask "would these two fight?" before, say, spawning them
// together or routing a follower past a camp. Composes Actor.Factions with the
// faction reaction records.
public static class FactionRelations
{
    // How `observer`'s factions regard `target`'s: Enemy if any pairing is hostile
    // (hostility dominates), otherwise the friendliest pairing, else Neutral. This
    // is directional -- how the observer sees the target.
    public static CombatReaction ReactionToward(Actor observer, Actor target)
    {
        IReadOnlyList<FactionMembership> mine = observer.Factions;
        IReadOnlyList<FactionMembership> theirs = target.Factions;

        CombatReaction best = CombatReaction.Neutral;
        foreach (FactionMembership a in mine)
            foreach (FactionMembership b in theirs)
            {
                CombatReaction reaction = a.Faction.ReactionToward(b.Faction);
                if (reaction == CombatReaction.Enemy) return CombatReaction.Enemy;
                if (reaction > best) best = reaction;  // Ally/Friend over Neutral
            }
        return best;
    }

    // True if either regards the other as an enemy -- the pair would fight.
    public static bool AreEnemies(Actor a, Actor b) =>
        ReactionToward(a, b) == CombatReaction.Enemy || ReactionToward(b, a) == CombatReaction.Enemy;

    // True if their factions are allied or friendly and neither is hostile.
    public static bool AreAllied(Actor a, Actor b) =>
        !AreEnemies(a, b) &&
        (ReactionToward(a, b) >= CombatReaction.Ally || ReactionToward(b, a) >= CombatReaction.Ally);
}
