using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the assistance resolver: a hostile bond or an enemy faction never helps,
// HelpsNobody stays out, HelpsAllies backs anyone not on the outs, and
// HelpsFriendsAndAllies needs a real friendship or an allied faction.
public static class NpcMoraleTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        // An enemy faction never helps, however eager the flag.
        check.That("an enemy faction never helps",
                   !NpcMorale.WillAssist(Assistance.HelpsAllies, CombatReaction.Enemy,
                                         NpcMorale.FriendRank));
        // A hostile bond never helps either.
        check.That("a hostile bond never helps",
                   !NpcMorale.WillAssist(Assistance.HelpsAllies, CombatReaction.Neutral,
                                         NpcMorale.HostileRank));

        // HelpsNobody stays out regardless of a warm bond.
        check.That("helps-nobody stays out",
                   !NpcMorale.WillAssist(Assistance.HelpsNobody, CombatReaction.Friend,
                                         NpcMorale.FriendRank));

        // HelpsAllies backs anyone not on the outs, even a bare acquaintance.
        check.That("helps-allies backs an acquaintance",
                   NpcMorale.WillAssist(Assistance.HelpsAllies, CombatReaction.Neutral, 0));

        // HelpsFriendsAndAllies needs a real friendship...
        check.That("helps-friends needs a friendship",
                   NpcMorale.WillAssist(Assistance.HelpsFriendsAndAllies, CombatReaction.Neutral,
                                        NpcMorale.FriendRank));
        // ...or an allied faction...
        check.That("helps-friends accepts an allied faction",
                   NpcMorale.WillAssist(Assistance.HelpsFriendsAndAllies, CombatReaction.Ally, 0));
        // ...but a bare neutral acquaintance is not enough.
        check.That("helps-friends declines a bare acquaintance",
                   !NpcMorale.WillAssist(Assistance.HelpsFriendsAndAllies, CombatReaction.Neutral, 0));

        // The actor-backed overload reads the faction reaction off the records.
        const ulong helper = 0x80, player = 0x14, guild = 0x800;
        fake.SetActorFactions(helper, (guild, 0));
        fake.SetActorFactions(player, (guild, 0));
        fake.SetFactionReaction(guild, guild, (int)CombatReaction.Ally);
        check.That("actor overload reads an allied guild",
                   NpcMorale.WillAssist(Actor.From(helper), Actor.From(player),
                                        Assistance.HelpsFriendsAndAllies, 0));

        // A guild-mate gone rival is refused by the same overload.
        check.That("actor overload refuses a rival",
                   !NpcMorale.WillAssist(Actor.From(helper), Actor.From(player),
                                         Assistance.HelpsAllies, NpcMorale.HostileRank));

        Native.Backend = null;
    }
}
