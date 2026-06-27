using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers faction-derived hostility: typed faction reactions, and the actor-level
// "would these two fight?" check that combines their factions' reactions.
public static class FactionRelationsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong bandit = 0x10, guard = 0x11, citizen = 0x12;
        const ulong banditFac = 0x100, guardFac = 0x101, townFac = 0x102;
        fake.SetActorFactions(bandit, (banditFac, 0));
        fake.SetActorFactions(guard, (guardFac, 0));
        fake.SetActorFactions(citizen, (townFac, 0));

        // Bandits and guards are enemies; guards and townsfolk are allies.
        fake.SetFactionReaction(banditFac, guardFac, (int)CombatReaction.Enemy);
        fake.SetFactionReaction(guardFac, banditFac, (int)CombatReaction.Enemy);
        fake.SetFactionReaction(guardFac, townFac, (int)CombatReaction.Ally);
        fake.SetFactionReaction(townFac, guardFac, (int)CombatReaction.Ally);

        // A faction's reaction reads back typed.
        check.Equal("faction reaction is typed", CombatReaction.Enemy,
                    Faction.From(banditFac).ReactionToward(Faction.From(guardFac)));

        // Actors inherit their factions' hostility.
        check.Equal("a bandit regards a guard as an enemy", CombatReaction.Enemy,
                    FactionRelations.ReactionToward(Actor.From(bandit), Actor.From(guard)));
        check.That("bandit and guard are enemies",
                   FactionRelations.AreEnemies(Actor.From(bandit), Actor.From(guard)));
        check.That("enemies are not allied",
                   !FactionRelations.AreAllied(Actor.From(bandit), Actor.From(guard)));

        // Allied factions make their members allies.
        check.That("guard and citizen are allied",
                   FactionRelations.AreAllied(Actor.From(guard), Actor.From(citizen)));
        check.That("allies are not enemies",
                   !FactionRelations.AreEnemies(Actor.From(guard), Actor.From(citizen)));

        // Unrelated factions leave actors neutral.
        check.Equal("a bandit and a townsperson are neutral", CombatReaction.Neutral,
                    FactionRelations.ReactionToward(Actor.From(bandit), Actor.From(citizen)));
        check.That("the neutral pair are not enemies",
                   !FactionRelations.AreEnemies(Actor.From(bandit), Actor.From(citizen)));
        check.That("the neutral pair are not allied",
                   !FactionRelations.AreAllied(Actor.From(bandit), Actor.From(citizen)));

        Native.Backend = null;
    }
}
