using System.Linq;
using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers authored faction enumeration: an actor lists every faction its record
// places it in, with the rank held, so mods can read guards, bandits and the like.
public static class FactionsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong guard = 0x100, crimeFaction = 0x200, cityGuards = 0x201, lonely = 0x101;
        fake.SetActorFactions(guard, (crimeFaction, 0), (cityGuards, 2));

        var factions = Actor.From(guard).Factions;
        check.Equal("all memberships are listed", 2, factions.Count);

        var byHandle = factions.ToDictionary(f => f.Faction.Handle, f => f.Rank);
        check.That("includes the crime faction", byHandle.ContainsKey(crimeFaction));
        check.Equal("rank is read", 2, byHandle[cityGuards]);
        check.Equal("default rank is read", 0, byHandle[crimeFaction]);

        // Mods can filter actors by faction membership.
        bool isCityGuard = Actor.From(guard).Factions.Any(f => f.Faction.Handle == cityGuards);
        check.That("a mod can test membership by enumeration", isCityGuard);

        // An actor authored into nothing lists no factions.
        check.Equal("a faction-less actor lists none", 0, Actor.From(lonely).Factions.Count);

        Native.Backend = null;
    }
}
