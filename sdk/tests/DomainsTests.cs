using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// recreation can run several games at once; a mod reaches each through the
// Domains registry and a GameWorld. These tests prove a mod reads Skyrim and
// Fallout content side by side, each routed to its own engine backend, with no
// cross-talk: a form id present in one game resolves only through that game.
internal static class DomainsTests
{
    public static void Run(Check check)
    {
        var skyrim = new FakeBackend();
        skyrim.SetName(0x7, "Prisoner");
        skyrim.SetFormType(0x7, 44);

        var fallout = new FakeBackend();
        fallout.SetName(0x24A3B2, "10mm");
        fallout.SetFormType(0x24A3B2, 42);

        var skyrimWorld = new GameWorld("Skyrim Special Edition", skyrim);
        var falloutWorld = new GameWorld("Fallout 4", fallout);

        Domains.Clear();
        Domains.Register(skyrimWorld, isPrimary: true);
        Domains.Register(falloutWorld, isPrimary: false);

        check.Equal("two domains registered", 2, Domains.Count);
        check.That("primary is skyrim", Domains.Primary == skyrimWorld);
        check.That("lookup by name", Domains.Get("Fallout 4") == falloutWorld);
        check.That("lookup is case-insensitive", Domains.Get("fallout 4") == falloutWorld);
        check.That("unknown game is null", Domains.Get("Morrowind") == null);

        // Each world resolves its own records.
        check.Equal("skyrim reads its form", "Prisoner", skyrimWorld.GetForm(0x7).Name);
        check.Equal("fallout reads its form", "10mm", falloutWorld.GetForm(0x24A3B2).Name);
        check.Equal("fallout weapon type", 42, falloutWorld.GetForm(0x24A3B2).FormType);

        // No cross-talk: a record named in one game is unknown in the other.
        check.That("skyrim has no name for the fallout form",
                   string.IsNullOrEmpty(skyrimWorld.GetForm(0x24A3B2).Name));
        check.Equal("skyrim has no type for the fallout form", 0,
                    skyrimWorld.GetForm(0x24A3B2).FormType);
        check.That("fallout has no name for the skyrim form",
                   string.IsNullOrEmpty(falloutWorld.GetForm(0x7).Name));

        Domains.Clear();
        check.Equal("cleared", 0, Domains.Count);
    }
}
