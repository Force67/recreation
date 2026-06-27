using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers lycanthropy: only a werewolf transforms, beast form counts down in real
// time, feeding draws it out, it reverts on its own when the timer runs out, and
// a cure ends it.
public static class LycanthropyTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        Werewolf.Clear();
        EventBus.Clear();

        int transforms = 0, reverts = 0;
        using var s1 = EventBus.Subscribe<TransformedToBeast>(_ => transforms++);
        using var s2 = EventBus.Subscribe<RevertedToHuman>(_ => reverts++);

        Actor player = Game.Player;

        // Without the beast blood there is no transforming.
        check.That("a mortal cannot transform", !Werewolf.Transform(player));
        check.That("granting the beast blood", Werewolf.Bestow(player));
        check.That("now a werewolf", Werewolf.IsWerewolf(player));
        check.That("re-granting does nothing", !Werewolf.Bestow(player));

        // Taking beast form starts the base timer.
        check.That("transforming succeeds", Werewolf.Transform(player));
        check.That("now a beast", Werewolf.IsTransformed(player));
        check.Equal("base form length", Werewolf.BaseFormSeconds, Werewolf.RemainingSeconds(player));
        check.That("cannot transform twice", !Werewolf.Transform(player));
        check.Equal("one transform announced", 1, transforms);

        // The timer counts down; feeding adds time.
        Werewolf.Tick(100f);
        check.Equal("timer counted down", 50f, Werewolf.RemainingSeconds(player));
        check.That("feeding draws it out", Werewolf.Feed(player));  // +30
        check.Equal("feeding extended the form", 80f, Werewolf.RemainingSeconds(player));

        // A tick short of the end keeps the beast; the one that empties it reverts.
        Werewolf.Tick(80f);
        check.That("the beast reverted when time ran out", !Werewolf.IsTransformed(player));
        check.Equal("one revert announced", 1, reverts);
        check.That("feeding after reverting does nothing", !Werewolf.Feed(player));

        // Curing while transformed reverts first, then strips the blood.
        Werewolf.Transform(player);
        check.That("transformed again", Werewolf.IsTransformed(player));
        check.That("curing succeeds", Werewolf.Cure(player));
        check.That("reverted by the cure", !Werewolf.IsTransformed(player));
        check.That("no longer a werewolf", !Werewolf.IsWerewolf(player));
        check.Equal("the cure reverted the beast form", 2, reverts);

        Werewolf.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
