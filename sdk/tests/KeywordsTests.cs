using System.Linq;
using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers keyword enumeration: a form reports every keyword it carries, so mods can
// categorise and filter forms by them.
public static class KeywordsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong sword = 0x100, weapTypeSword = 0x200, vendorWeapon = 0x201, barren = 0x101;
        fake.SetHasKeyword(sword, weapTypeSword);
        fake.SetHasKeyword(sword, vendorWeapon);

        var keywords = Form.From(sword).Keywords;
        check.Equal("all keywords are listed", 2, keywords.Count);
        var handles = keywords.Select(k => k.Handle).ToHashSet();
        check.That("includes the weapon-type keyword", handles.Contains(weapTypeSword));
        check.That("includes the vendor keyword", handles.Contains(vendorWeapon));

        // HasKeyword still answers a single membership query.
        check.That("HasKeyword agrees", Form.From(sword).HasKeyword(Form.From(weapTypeSword)));
        check.That("HasKeyword rejects an absent one", !Form.From(sword).HasKeyword(Form.From(0x999)));

        // A form with no keywords lists none.
        check.Equal("a barren form has no keywords", 0, Form.From(barren).Keywords.Count);

        // Mods can filter forms by keyword membership.
        bool isWeapon = Form.From(sword).Keywords.Any(k => k.Handle == vendorWeapon);
        check.That("a mod can filter by keyword", isWeapon);

        Native.Backend = null;
    }
}
