using System.Collections.Generic;
using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers vampirism progression: turning starts at stage one, hunger advances the
// stage a day at a time up to four, feeding resets it, and curing ends it, with a
// stage-change event for every transition.
public static class VampirismTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend { Player = 0x14 };
        Native.Backend = fake;
        Vampirism.Clear();
        EventBus.Clear();
        fake.GameTime = 100.0f;

        var stages = new List<(int Previous, int Stage)>();
        using var sub = EventBus.Subscribe<VampireStageChanged>(e => stages.Add((e.Previous, e.Stage)));

        Actor player = Game.Player;
        check.That("not a vampire to begin with", !Vampirism.IsVampire(player));
        check.Equal("no stage when mortal", 0, Vampirism.StageOf(player));

        // Turning makes a stage-one vampire and announces 0 -> 1.
        check.That("infection turns the actor", Vampirism.Infect(player));
        check.That("now a vampire", Vampirism.IsVampire(player));
        check.Equal("starts at stage one", 1, Vampirism.StageOf(player));
        check.That("re-infecting does nothing", !Vampirism.Infect(player));
        check.Equal("turning announced 0 -> 1", (0, 1), stages[^1]);

        // A day of hunger advances to stage two; three days reach the cap at four.
        fake.GameTime = 101.0f;
        Vampirism.Refresh();
        check.Equal("a day unfed is stage two", 2, Vampirism.StageOf(player));
        check.Equal("advance announced 1 -> 2", (1, 2), stages[^1]);

        fake.GameTime = 105.0f;  // five days: clamps to the worst stage
        Vampirism.Refresh();
        check.Equal("starved tops out at four", 4, Vampirism.StageOf(player));
        check.Equal("advance announced 2 -> 4", (2, 4), stages[^1]);

        // Feeding resets hunger to stage one.
        Vampirism.Feed(player);
        check.Equal("feeding drops back to stage one", 1, Vampirism.StageOf(player));
        check.Equal("feeding announced 4 -> 1", (4, 1), stages[^1]);

        // Refresh with nothing changed announces nothing more.
        int before = stages.Count;
        Vampirism.Refresh();
        check.Equal("a steady stage announces nothing", before, stages.Count);

        // Curing ends vampirism and announces a return to mortal.
        check.That("curing succeeds", Vampirism.Cure(player));
        check.That("no longer a vampire", !Vampirism.IsVampire(player));
        check.Equal("cure announced 1 -> 0", (1, 0), stages[^1]);
        check.That("curing again does nothing", !Vampirism.Cure(player));

        Vampirism.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }
}
