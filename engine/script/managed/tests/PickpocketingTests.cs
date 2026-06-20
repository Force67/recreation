using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers pickpocketing: the odds rise with skill and fall with an item's weight,
// value and quantity (clamped at both ends), and an attempt moves the goods only
// when the roll beats the chance, leaving a failed attempt detected.
public static class PickpocketingTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;

        const ulong gem = 0x700;
        fake.SetWeight(gem, 0f);
        fake.SetGoldValue(gem, 50);

        // chance = 10 + 0.8*50 - 1*(3*0 + 0.1*50) = 45
        check.Equal("skill and value set the odds", 45, PickpocketOdds.Chance(50, Form.From(gem)));

        // The clamps hold at both ends: a worthless trinket at fortified skill would
        // exceed the ceiling (10 + 0.8*110 = 98) and a heavy haul falls below zero.
        const ulong trinket = 0x702, boulder = 0x701;
        fake.SetWeight(trinket, 0f);
        fake.SetGoldValue(trinket, 0);
        fake.SetWeight(boulder, 50f);
        check.Equal("a master tops out at the ceiling", 90, PickpocketOdds.Chance(110, Form.From(trinket)));
        check.Equal("a heavy haul floors at zero", 0, PickpocketOdds.Chance(0, Form.From(boulder)));

        // Taking more multiplies the penalty.
        check.Equal("two gems are harder than one", 40, PickpocketOdds.Chance(50, Form.From(gem), count: 2));

        const ulong thiefId = 0x14, mark = 0x300;
        var thief = Actor.From(thiefId);
        var victim = Actor.From(mark);
        fake.SetValue(thiefId, ActorValue.Pickpocket, current: 50, baseValue: 50);
        victim.AddItem(Form.From(gem), 3);

        // A roll under the chance lifts the item to the thief.
        PickpocketResult win = Pickpocket.Attempt(thief, victim, Form.From(gem), roll: 0);
        check.That("a winning roll succeeds", win.Success);
        check.Equal("it resolved against the right chance", 45, win.Chance);
        check.Equal("the thief gains the gem", 1, thief.GetItemCount(Form.From(gem)));
        check.Equal("the victim is one lighter", 2, victim.GetItemCount(Form.From(gem)));

        // A roll at or above the chance fails and is detected; nothing moves.
        PickpocketResult loss = Pickpocket.Attempt(thief, victim, Form.From(gem), roll: 45);
        check.That("a losing roll fails", !loss.Success);
        check.That("a failed attempt is caught", loss.Caught);
        check.Equal("the thief gains nothing", 1, thief.GetItemCount(Form.From(gem)));
        check.Equal("the victim keeps the rest", 2, victim.GetItemCount(Form.From(gem)));

        // Lifting more than is carried is capped to what the victim holds.
        PickpocketResult greedy = Pickpocket.Attempt(thief, victim, Form.From(gem), roll: 0, count: 10);
        check.That("the over-reach still succeeds", greedy.Success);
        check.Equal("only what was carried moved", 3, thief.GetItemCount(Form.From(gem)));

        // Nothing to take is not an attempt at all.
        PickpocketResult empty = Pickpocket.Attempt(thief, victim, Form.From(gem), roll: 0);
        check.That("an empty pocket is no attempt", !empty.Attempted);
        check.That("and not counted as caught", !empty.Caught);

        Native.Backend = null;
    }
}
