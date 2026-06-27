using Recreation;
using Recreation.Games.Fallout;
using Recreation.Interop;

namespace Recreation.Tests;

// The Fallout entry point reaches Fallout 4 content while the primary world is
// some other game (here a Skyrim stand-in), proving a mod consumes both at once.
internal static class FalloutTests
{
    public static void Run(Check check)
    {
        Domains.Clear();

        // Only the primary world loaded: Fallout is not available.
        var primary = new FakeBackend();
        Domains.Register(new GameWorld("Skyrim Special Edition", primary), isPrimary: true);
        check.That("fallout absent when not loaded", !Fallout.Loaded);
        check.That("fallout world null when absent", Fallout.World == null);
        check.That("getform safe when absent", !Fallout.GetForm(0x24A3B2).Exists);

        // Add the Fallout 4 domain alongside it.
        var fo4 = new FakeBackend();
        fo4.SetName(0x24A3B2, "10mm");
        fo4.SetFormType(0x24A3B2, 42);
        Domains.Register(new GameWorld(Fallout.GameName, fo4), isPrimary: false);

        check.That("fallout available once loaded", Fallout.Loaded);
        check.Equal("fallout reads its form", "10mm", Fallout.GetForm(0x24A3B2).Name);
        check.Equal("fallout NameOf", "10mm", Fallout.NameOf(0x24A3B2));
        check.Equal("fallout weapon type", 42, Fallout.GetForm(0x24A3B2).FormType);

        Domains.Clear();
    }
}
