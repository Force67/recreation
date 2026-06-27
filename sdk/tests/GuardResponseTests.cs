using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the guard confrontation layer: while clear nothing happens, a wanted
// player near an authority NPC is confronted once (with a "Halt!" notification and
// an event), a non-authority bystander is ignored, and paying the bounty down
// stands the guard down.
public static class GuardResponseTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        const ulong player = 0x14, guard = 0x30, citizen = 0x31;
        const ulong crimeFaction = 0x300, plainFaction = 0x301;
        fake.Player = player;
        fake.SetFactionFlags(crimeFaction, 0x40);   // 0x40 = Track Crime: an authority
        fake.SetFactionFlags(plainFaction, 0x00);
        fake.SetActorFactions(guard, (crimeFaction, 0));
        fake.SetActorFactions(citizen, (plainFaction, 0));

        // Everyone stands in range of the player.
        fake.SetPosition(player, 0, 0, 0);
        fake.SetPosition(guard, 100, 0, 0);
        fake.SetPosition(citizen, 50, 0, 0);

        var confrontations = new List<GuardConfrontation>();
        EventBus.Subscribe<GuardConfrontation>(confrontations.Add);

        var wanted = new WantedLevel();
        var guards = new GuardResponse(wanted) { ScanInterval = 1f };
        ModHost.Register(wanted);
        ModHost.Register(guards);

        // Clean player: a scan confronts no one.
        ModHost.Tick(1f);
        check.That("no confrontation while clear", !guards.Confronting);
        check.Equal("no confrontation events while clear", 0, confrontations.Count);

        // The player commits a reported crime and is now wanted.
        wanted.Report(50);
        check.That("player is wanted", wanted.IsWanted);

        // The next scan confronts the guard, not the citizen, once.
        ModHost.Tick(1f);
        check.That("the guard is confronting", guards.Confronting);
        check.Equal("the confronting actor is the guard", guard, guards.ActiveGuard);
        check.Equal("one confrontation opened", 1, confrontations.Count);
        check.That("the confrontation is open", confrontations[0].Confronting);
        check.Equal("the event names the guard", guard, confrontations[0].GuardHandle);
        check.That("halt was notified",
                   fake.Notifications.Contains("Halt! You've committed crimes against the law."));

        // A second scan does not re-open the standoff.
        ModHost.Tick(1f);
        check.Equal("the standoff is not re-opened", 1, confrontations.Count);

        // Paying the bounty down clears the heat; the next scan stands the guard down.
        wanted.Clear();
        ModHost.Tick(1f);
        check.That("the guard stood down", !guards.Confronting);
        check.Equal("a standdown event fired", 2, confrontations.Count);
        check.That("the standdown is a close", !confrontations[1].Confronting);

        // A wanted player with only a non-authority bystander near is not confronted.
        fake.SetPosition(guard, 100000, 0, 0);  // the guard leaves
        wanted.Report(50);
        ModHost.Tick(1f);
        check.That("a citizen does not confront", !guards.Confronting);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
