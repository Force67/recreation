using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the crime/bounty layer: finding the hold an actor reports to, reporting a
// crime, and paying the bounty off.
public static class CrimeTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong guard = 0x10, wolf = 0x11;
        const ulong whiterunCrime = 0x100, guardFaction = 0x101;
        fake.SetFactionFlags(whiterunCrime, 0x40);  // 0x40 = Track Crime
        fake.SetFactionFlags(guardFaction, 0x00);
        fake.SetActorFactions(guard, (guardFaction, 0), (whiterunCrime, 0));
        fake.SetActorFactions(wolf);  // a creature reports to no one

        // The crime faction is recognised, and the guard reports to it.
        check.That("the crime faction is flagged", Faction.From(whiterunCrime).IsCrimeFaction);
        check.That("a plain faction is not a crime faction", !Faction.From(guardFaction).IsCrimeFaction);
        check.Equal("a guard reports to the hold's crime faction", whiterunCrime,
                    Crime.FactionOf(Actor.From(guard)).Handle);
        check.That("a creature reports to no hold", !Crime.FactionOf(Actor.From(wolf)).Exists);

        // Reporting a crime raises the bounty there.
        Faction hold = Faction.From(whiterunCrime);
        check.That("no bounty to start", !Crime.IsWanted(hold));
        Crime.Report(Actor.From(guard), 40);
        check.Equal("bounty was added", 40, Crime.BountyIn(hold));
        check.That("the player is now wanted", Crime.IsWanted(hold));

        Crime.Report(hold, 1000);  // another crime
        check.Equal("bounty accumulates", 1040, Crime.BountyIn(hold));

        // Paying it off clears the bounty.
        Crime.Pay(hold);
        check.Equal("bounty cleared", 0, Crime.BountyIn(hold));
        check.That("no longer wanted", !Crime.IsWanted(hold));

        // Reporting to an actor with no crime faction does nothing.
        Crime.Report(Actor.From(wolf), 50);  // must not throw
        check.That("a holdless report is harmless", !Crime.IsWanted(hold));

        Native.Backend = null;
    }
}
