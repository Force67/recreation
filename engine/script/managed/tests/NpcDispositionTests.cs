using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the disposition resolver: an enemy faction is hostile outright, a wanted
// player turns a law-keeper hostile (but not an outlaw), a personal bond tips an
// otherwise neutral read, and the actor-backed overload reads the faction reaction
// off the records.
public static class NpcDispositionTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // --- the pure resolver ----------------------------------------------------
        // An enemy faction pairing is hostile regardless of anything else.
        check.Equal("enemy faction is hostile", Disposition.Hostile,
                    NpcDisposition.Toward(CombatReaction.Enemy, NpcDisposition.FriendlyRank,
                                          WantedBand.Clear));

        // A clean neutral read is neutral.
        check.Equal("neutral all round is neutral", Disposition.Neutral,
                    NpcDisposition.Toward(CombatReaction.Neutral, 0, WantedBand.Clear));

        // A wanted player turns a law-respecting NPC hostile even with goodwill.
        check.Equal("wanted turns a law-keeper hostile", Disposition.Hostile,
                    NpcDisposition.Toward(CombatReaction.Friend, NpcDisposition.FriendlyRank,
                                          WantedBand.Wanted));

        // An outlaw who does not respect the law ignores the wanted level.
        check.Equal("an outlaw ignores the wanted level", Disposition.Friendly,
                    NpcDisposition.Toward(CombatReaction.Neutral, NpcDisposition.FriendlyRank,
                                          WantedBand.Hunted, respectsLaw: false));

        // A mere Suspected band does not flip a law-keeper hostile.
        check.Equal("suspected does not turn a law-keeper", Disposition.Neutral,
                    NpcDisposition.Toward(CombatReaction.Neutral, 0, WantedBand.Suspected));

        // A friend bond is friendly; a foe bond is hostile.
        check.Equal("a friend bond is friendly", Disposition.Friendly,
                    NpcDisposition.Toward(CombatReaction.Neutral, NpcDisposition.FriendlyRank,
                                          WantedBand.Clear));
        check.Equal("a foe bond is hostile", Disposition.Hostile,
                    NpcDisposition.Toward(CombatReaction.Neutral, NpcDisposition.HostileRank,
                                          WantedBand.Clear));

        // An allied faction warms a stranger.
        check.Equal("an allied faction warms a stranger", Disposition.Friendly,
                    NpcDisposition.Toward(CombatReaction.Ally, 0, WantedBand.Clear));

        check.That("IsHostile reads the value", NpcDisposition.IsHostile(Disposition.Hostile));
        check.That("IsFriendly reads the value", NpcDisposition.IsFriendly(Disposition.Friendly));

        // --- the actor-backed overload -------------------------------------------
        const ulong npc = 0x20, player = 0x14;
        const ulong townFaction = 0x200, banditFaction = 0x201;
        fake.SetActorFactions(npc, (townFaction, 0));
        fake.SetActorFactions(player, (townFaction, 0));
        fake.SetFactionReaction(townFaction, townFaction, (int)CombatReaction.Friend);

        var wanted = new WantedLevel();
        // A clean, fellow-townsfolk player with a friendly faction reads friendly.
        check.Equal("actor overload reads the records", Disposition.Friendly,
                    NpcDisposition.Toward(Actor.From(npc), Actor.From(player), 0, wanted));

        // Now the player is wanted: the law-keeping townsperson turns hostile.
        wanted.Report(50);
        check.Equal("actor overload folds in the heat", Disposition.Hostile,
                    NpcDisposition.Toward(Actor.From(npc), Actor.From(player), 0, wanted));

        // A null meter is treated as clear heat.
        check.Equal("a null meter is clear heat", Disposition.Friendly,
                    NpcDisposition.Toward(Actor.From(npc), Actor.From(player), 0, null));

        // The lifted faction fold matches FactionRelations' Enemy-dominates rule.
        fake.SetFactionReaction(banditFaction, townFaction, (int)CombatReaction.Enemy);
        fake.SetActorFactions(npc, (townFaction, 0), (banditFaction, 0));
        check.Equal("enemy pairing dominates the fold", CombatReaction.Enemy,
                    NpcDisposition.FactionReactionToward(Actor.From(npc), Actor.From(player)));

        Native.Backend = null;
    }
}
