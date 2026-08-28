using System.Collections.Generic;
using Recreation;
using Recreation.Games.SkyrimCartRacing;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises Super Cart Xtreme Racing: the pure RaceSim scoring/gating/boost
// machine directly, then the CartRacingBehaviour end to end against the fake
// backend (mount, countdown, drive mapping, checkpoint gating, wrong way,
// respawn, finish).
public static class SkyrimCartRacingTests
{
    private const ulong Player = 0x14;

    public static void Run(Check check)
    {
        RaceSimLogic(check);
        Behaviour(check);
    }

    private static void RaceSimLogic(Check check)
    {
        // --- construction guards ---
        check.That("rejects a course with one checkpoint",
                    Throws(() => new RaceSim(new[] { new Vector3(0, 0, 0) }, 1)));
        check.That("rejects zero laps", Throws(() => new RaceSim(TwoCheckpoints(), 0)));

        // --- progress and gating across two laps over a two-mark course ---
        var race = new RaceSim(TwoCheckpoints(), 2);
        check.Equal("starts on lap 1", 1, race.Lap);
        check.Equal("expects the first checkpoint", 0, race.CheckpointIndex);
        check.Equal("respawns at the start mark", new Vector3(0, 0, 0), race.RespawnPosition);

        check.Equal("clearing CP0 moves on (no milestone)", false, race.EnterCheckpoint(0));
        check.Equal("moves to the second checkpoint", 1, race.CheckpointIndex);
        check.Equal("respawn is the cleared checkpoint", new Vector3(0, 0, 0),
                    race.RespawnPosition);
        check.That("clearing the second checkpoint completes lap 1", race.EnterCheckpoint(1));
        check.Equal("second lap", 2, race.Lap);
        check.Equal("course index wraps", 0, race.CheckpointIndex);
        check.Equal("respawn is the last checkpoint of lap 1", new Vector3(100, 0, 0),
                    race.RespawnPosition);

        check.Equal("lap 2 first mark is not a milestone", false, race.EnterCheckpoint(0));
        check.That("second lap complete is the finish", race.EnterCheckpoint(1));
        check.That("race is done", race.Done);

        // --- the done race stops scoring ---
        float before = race.TotalTime;
        race.Update(1f, boosting: true);
        check.Equal("times freeze after the finish", before, race.TotalTime);

        // --- wrong way: out-of-order check-in does not move progress ---
        var wrong = new RaceSim(TwoCheckpoints(), 1);
        check.That("a behind checkpoint does not move", !wrong.EnterCheckpoint(1));  // CP1 before CP0
        check.That("flagged wrong", wrong.Wrong);
        check.Equal("still expects the first checkpoint", 0, wrong.CheckpointIndex);

        // --- boost meter drains, floors, and refills ---
        race = new RaceSim(TwoCheckpoints(), 1);
        race.BoostDrainPerSecond = 0.5f;
        race.BoostRegenPerSecond = 0.25f;
        race.Update(1f, boosting: true);
        check.Equal("boost drains while held", 0.5f, race.Boost);
        race.Update(2f, boosting: true);
        check.Equal("boost clamps at zero", 0f, race.Boost);
        race.Update(4f, boosting: false);
        check.Equal("boost refills when released", 1f, race.Boost);

        // --- lap clock resets but total keeps a running sum, and best lap tracks ---
        race = new RaceSim(TwoCheckpoints(), 3);
        race.BoostRegenPerSecond = 0f;  // not the concern here
        race.Update(1f, boosting: false);
        race.EnterCheckpoint(0);
        race.Update(2f, boosting: false);
        race.EnterCheckpoint(1);  // lap 1 in 3s
        check.Equal("lap clock reset", 0f, race.LapTime);
        check.Equal("best lap is the first lap", 3f, race.BestLap);
        race.Update(2f, boosting: false);
        race.EnterCheckpoint(0);
        race.EnterCheckpoint(1);  // lap 2 in 2s, faster
        check.Equal("best lap improves", 2f, race.BestLap);
        check.Equal("total counts both laps", 5f, race.TotalTime);
    }

    private static void Behaviour(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        Notify.Clear();

        // A small ring course around the player at the origin: 4 checkpoints on a
        // 100-unit radius so positions are round numbers.
        fake.SetPosition(Player, 0, 0, 0);
        var behaviour = new CartRacingBehaviour
        {
            Laps = 1,
            CheckpointCount = 4,
            CourseRadius = 100f,
            CheckpointReach = 30f,
            BoostDrainPerSecond = 0.5f,
            BoostRegenPerSecond = 0.2f,
        };
        ModHost.Register(behaviour);

        // --- not riding: no race state and no drive ---
        ModHost.Tick(1f);
        check.Equal("no drive before mount", Plot(0f, 0f), fake.LastVehicleDrive);

        // --- mount starts the countdown; no driving until it runs out ---
        fake.VehicleRiding = true;
        for (int i = 0; i < 3; i++) ModHost.Tick(1f);  // 3.5 -> 0.5
        check.Equal("still counting down, no drive", Plot(0f, 0f), fake.LastVehicleDrive);
        ModHost.Tick(1f);                              // crosses zero: GO
        check.That("go fired", System.Linq.Enumerable.Any(Notify.Active, n => n.Text == "GO!"));
        ModHost.Tick(0.1f);                            // first live race frame, gauges up
        check.That("gauges mounted", fake.Gauges.ContainsKey("cart_speed"));

        // --- W/A steer + throttle map to Vehicle.Drive ---
        fake.SetHeld(Key.W, Key.A);
        fake.VehicleSpeed = 5f;
        ModHost.Tick(0.1f);
        check.Equal("W+A drives at full throttle, left steer", Plot(1f, 1f), fake.LastVehicleDrive);

        // --- boost burns into an overspeed throttle while the meter lasts ---
        fake.SetHeld(Key.W, Key.Space);
        fake.VehicleSpeed = 10f;   // moving
        ModHost.Tick(0.1f);
        check.Equal("boost pushes throttle past one", 1.7f, fake.LastVehicleDrive.Throttle);

        // --- no throttle plus brake (S) holds the horse ---
        fake.SetHeld(Key.S);
        ModHost.Tick(0.1f);
        check.Equal("W stops the cart", 0f, fake.LastVehicleDrive.Throttle);

        // --- checkpoint gating moves through the ring ---
        fake.SetHeld(Key.W);
        MoveTo(fake, 100, 0, 0);   // CP0
        ModHost.Tick(0.1f);
        MoveTo(fake, 0, 0, 100);   // CP1
        ModHost.Tick(0.1f);
        MoveTo(fake, -100, 0, 0);  // CP2
        ModHost.Tick(0.1f);
        MoveTo(fake, 0, 0, -100);  // CP3 closes lap 1 of 1 -> finish
        ModHost.Tick(0.1f);

        // A done race stops driving (throttle 0, steer 0 implied by hold) and
        // shows the finish message.
        fake.SetHeld(Key.W);
        MoveTo(fake, 0, 0, 0);
        ModHost.Tick(0.1f);
        check.Equal("finished race holds the horse", 0f, fake.LastVehicleDrive.Throttle);
        check.That("finish is announced",
                   System.Linq.Enumerable.Any(Notify.Active, n => n.Text.Contains("FINISH")));

        // --- wrong way + R respawn back to the last checkpoint ---
        ModHost.Shutdown();
        Native.Backend = null;

        fake = new FakeBackend();
        Native.Backend = fake;
        Notify.Clear();
        fake.VehicleRiding = true;
        behaviour = new CartRacingBehaviour
        {
            Laps = 2,
            CheckpointCount = 4,
            CourseRadius = 100f,
            CheckpointReach = 30f,
        };
        ModHost.Register(behaviour);
        for (int i = 0; i < 4; i++) ModHost.Tick(1f);  // through the countdown

        MoveTo(fake, 100, 0, 0);    // CP0
        ModHost.Tick(0.1f);
        MoveTo(fake, -100, 0, 0);   // CP2, out of order -> wrong way
        ModHost.Tick(0.1f);
        check.That("wrong-way toast",
                   System.Linq.Enumerable.Any(Notify.Active, n => n.Text.Contains("WRONG")));

        EventBus.Publish(new KeyPressed(Key.R));
        check.Equal("R respawns to the last checkpoint", 100f,
                    fake.LastVehicleMove?.X ?? 0f);

        ModHost.Shutdown();
        Native.Backend = null;
    }

    private static Vector3[] TwoCheckpoints() =>
        new[] { new Vector3(0, 0, 0), new Vector3(100, 0, 0) };

    private static bool Throws(System.Action action)
    {
        try { action(); }
        catch { return true; }
        return false;
    }

    private static (float Steer, float Throttle) Plot(float steer, float throttle) =>
        (steer, throttle);
    private static void MoveTo(FakeBackend b, int x, int y, int z) => b.SetPosition(Player, x, y, z);
}