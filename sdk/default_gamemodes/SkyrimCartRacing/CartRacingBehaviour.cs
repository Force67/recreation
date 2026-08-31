using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Games.SkyrimCartRacing;

// Turns a mounted cart ride into a checkpoint race. On mount it generates a
// course ring around the player, counts down, then drives the ridden cart
// through the cart drive kit each frame: steer and throttle from held keys,
// boost from the meter, checkpoint gates and wrong-way detection, manual (R)
// and automatic (persistent wrong way) respawn to the last checkpoint cleared,
// and a win toast with the lap records when the final checkpoint goes by.
public sealed class CartRacingBehaviour : GameBehaviour
{
    public int Laps { get; set; } = 3;
    public int CheckpointCount { get; set; } = 6;
    public float CourseRadius { get; set; } = 4000f;      // game units from the mount point
    public float CheckpointReach { get; set; } = 250f;    // game units, how close counts
    public float BoostDrainPerSecond { get; set; } = 0.25f;
    public float BoostRegenPerSecond { get; set; } = 0.15f;
    public float WrongWayRespawnSeconds { get; set; } = 5f;

    // The top of the boost overspeed (throttle > 1), as the C++ drive accepts it.
    private const float BoostThrottle = 1.7f;
    private const float TopSpeed = 18f;  // m/s, matches the host's drive cap

    private RaceSim? _race;
    private bool _riding;
    private float _countdown = -1f;  // >0 counting down, 0 go, <0 idle
    private float _wrongTime;
    private EventBus.Subscription? _keySub;

    protected override void OnStart()
    {
        // Manual respawn: back to the last checkpoint cleared. Works mid-race
        // and is a no-op otherwise.
        _keySub = EventBus.Subscribe<KeyPressed>(e =>
        {
            if (e.Key == Key.R && _race is { Done: false } && _riding)
                Respawn();
        });
    }

    protected override void OnUpdate(float deltaTime)
    {
        bool riding = Vehicle.Riding;
        if (riding && !_riding)
            StartRace();
        _riding = riding;

        if (!riding)
        {
            if (_race != null) EndRace();
            return;
        }
        if (_race is not { } race)
            return;

        if (_countdown > 0f)
        {
            _countdown -= deltaTime;
            if (_countdown <= 0f) Notify.Show("GO!", NoticeKind.Success, 1.5f);
            return;
        }
        if (race.Done)
        {
            Vehicle.Drive(0f, 0f);  // hold the horse while the win screen shows
            return;
        }

        float speed = Vehicle.Speed;
        bool boosting = Input.Held(Key.Space) && race.Boost > 0f && speed > 1f;
        race.Update(deltaTime, boosting);

        // W/S throttle and hold, A/D steer. Cruise sits at a light canter so the
        // race never stalls mid-straight.
        float steer = (Input.Held(Key.A) ? 1f : 0f) - (Input.Held(Key.D) ? 1f : 0f);
        float throttle = Input.Held(Key.W) ? 1f : Input.Held(Key.S) ? 0f : 0.35f;
        if (boosting) throttle = BoostThrottle;
        Vehicle.Drive(steer, throttle);

        RaceGeometry(race, deltaTime);
        HudGauges(race, speed);
    }

    private void StartRace()
    {
        Vector3 origin = Game.Player.Position;
        var checkpoints = new Vector3[CheckpointCount];
        for (int i = 0; i < CheckpointCount; i++)
        {
            double angle = Math.PI * 2.0 * i / CheckpointCount;
            checkpoints[i] = new Vector3(
                origin.X + (float)Math.Cos(angle) * CourseRadius,
                origin.Y,
                origin.Z + (float)Math.Sin(angle) * CourseRadius);
        }

        _race = new RaceSim(checkpoints, Laps)
        {
            BoostDrainPerSecond = BoostDrainPerSecond,
            BoostRegenPerSecond = BoostRegenPerSecond,
        };
        _countdown = 3.5f;
        _wrongTime = 0f;

        for (int i = 0; i < CheckpointCount; i++)
            Blips.CreateLocal($"cart_cp_{i}", checkpoints[i], $"CP {i + 1}",
                              BlipSprite.Objective, 0x3498db, shortRange: false);
        Notify.Show("Super Cart Xtreme Racing: follow the checkpoints!",
                    NoticeKind.Info, 5f);
    }

    private void EndRace()
    {
        if (_race is { Done: false } race)
            Notify.Show($"Race over. Best lap {race.BestLap:0.0}s, total {race.TotalTime:0.0}s.",
                        NoticeKind.Info, 6f);
        for (int i = 0; i < CheckpointCount; i++)
            Blips.Remove($"cart_cp_{i}");
        Hud.ClearGauge("cart_speed");
        Hud.ClearGauge("cart_boost");
        _race = null;
        _countdown = -1f;
    }

    private void RaceGeometry(RaceSim race, float deltaTime)
    {
        Vector3 pos = Game.Player.Position;

        // Scan every blip, not just the expected one, so standing over an
        // out-of-order checkpoint reads as a wrong way and a correct check-in
        // still advances the lap.
        bool wrong = false;
        for (int i = 0; i < race.Checkpoints.Count; i++)
        {
            if (Vector3.Distance(pos, race.Checkpoints[i]) > CheckpointReach)
                continue;
            if (i != race.CheckpointIndex)
            {
                wrong = true;
                break;
            }
            int lapCleared = race.Lap;
            if (race.EnterCheckpoint(i))
                OnMilestone(race, lapCleared);
            break;
        }

        // Wrong way: warn, then respawn if it goes on, since a cart can wedge
        // against a wall facing home.
        if (wrong)
        {
            _wrongTime += deltaTime;
            Notify.Show("WRONG WAY", NoticeKind.Warning, 1f);
            if (_wrongTime >= WrongWayRespawnSeconds)
                Respawn();
        }
        else
        {
            _wrongTime = 0f;
        }
    }

    private void OnMilestone(RaceSim race, int lapCleared)
    {
        if (race.Done)
        {
            Notify.Show($"FINISH! Best lap {race.BestLap:0.0}s, total {race.TotalTime:0.0}s.",
                        NoticeKind.Success, 8f);
            return;
        }
        Notify.Show($"Lap {lapCleared} of {race.TotalLaps} cleared.",
                    NoticeKind.Info, 3f);
    }

    private void Respawn() => Vehicle.MoveTo(_race!.RespawnPosition.X,
                                             _race.RespawnPosition.Y,
                                             _race.RespawnPosition.Z);

    private static void HudGauges(RaceSim race, float speed)
    {
        Hud.Gauge("cart_speed", Math.Clamp(speed / TopSpeed, 0f, 1f), $"{speed:0} m/s",
                  0x2ecc71);
        Hud.Gauge("cart_boost", race.Boost, "Boost", 0xe74c3c);
    }

    protected override void OnDestroy()
    {
        _keySub?.Dispose();
        if (_race != null) EndRace();
    }
}
